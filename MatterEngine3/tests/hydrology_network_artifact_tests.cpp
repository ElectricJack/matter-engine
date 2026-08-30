#include "check.h"

#include "hydrology/hydrology_field_artifact.h"
#include "hydrology/hydrology_network_artifact.h"
#include "hydrology/water_mesh_animation_artifact.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

hydrology::GameplayFieldLayout fixture_layout() {
    return {{-2.0f, 3.0f, 4.0f}, 0.5f, 2u, 2u};
}

hydrology::HydrologyFieldProduct fixture_runtime_product() {
    hydrology::HydrologyFieldProduct product{};
    product.kind = hydrology::HydrologyFieldProductKind::Runtime;
    product.layout = fixture_layout();
    product.gameplay = {
        {10.0f, 1.0f, 1.0f, 0.0f, 0.5f, true},
        {11.0f, 2.0f, 2.0f, 0.5f, 1.0f, true},
        {},
        {12.0f, 3.0f, 3.0f, 1.0f, 1.5f, true},
    };
    product.payload_digest = hydrology::hydrology_runtime_field_digest(
        product.layout, product.gameplay);
    return product;
}

hydrology::HydrologyFieldProduct fixture_presentation_product() {
    hydrology::HydrologyFieldProduct product{};
    product.kind = hydrology::HydrologyFieldProductKind::Presentation;
    product.layout = fixture_layout();
    product.presentation = {
        {0.0f, 0.0f, 0.2f, 0.3f, 0.4f,
         hydrology::RiverFeature::Current, true},
        {0.1f, -0.1f, 0.4f, 0.5f, 0.6f,
         hydrology::RiverFeature::Rapid, true},
        {},
        {-0.2f, 0.1f, 0.6f, 0.7f, 0.8f,
         hydrology::RiverFeature::Pool, true},
    };
    product.payload_digest = hydrology::hydrology_presentation_field_digest(
        product.layout, product.presentation);
    return product;
}

hydrology::HydrologyNetworkArtifact fixture_manifest(bool reversed = false) {
    const auto runtime = fixture_runtime_product();
    const auto presentation = fixture_presentation_product();
    hydrology::HydrologyNetworkArtifact manifest{};
    manifest.state = hydrology::HydrologyNetworkState::Ready;
    manifest.network_key = 101u;
    manifest.terrain_revision = 202u;
    manifest.runtime_field_digest = runtime.payload_digest;
    manifest.presentation_field_digest = presentation.payload_digest;
    manifest.field_products = {
        {hydrology::HydrologyFieldProductKind::Runtime,
         hydrology::hydrology_field_product_relative_path(
             hydrology::HydrologyFieldProductKind::Runtime,
             runtime.payload_digest), runtime.payload_digest},
        {hydrology::HydrologyFieldProductKind::Presentation,
         hydrology::hydrology_field_product_relative_path(
             hydrology::HydrologyFieldProductKind::Presentation,
             presentation.payload_digest), presentation.payload_digest},
    };
    manifest.sections = {
        {"upper", "sections/upper.mhyd", {}, 11u, 111u},
        {"lower", "sections/lower.mhyd", {"upper"}, 22u, 222u},
    };
    manifest.handoffs = {
        {"pool-one", "handoffs/pool-one.mhyd", {"upper", "lower"},
         33u, 333u},
    };
    manifest.topological_order = {"upper", "lower"};
    manifest.bounds_m = {{-10.0f, -4.0f, -20.0f},
                         {300.0f, 80.0f, 20.0f}};
    if (reversed) {
        std::reverse(manifest.sections.begin(), manifest.sections.end());
        std::reverse(manifest.handoffs.front().dependencies.begin(),
                     manifest.handoffs.front().dependencies.end());
    }
    return manifest;
}

hydrology::HydrologyNetworkArtifact animated_fixture_manifest() {
    auto manifest = fixture_manifest();
    manifest.section_animations = {
        {"upper", "hydrology/animations/upper-000000000000000b.mhwa",
         11u, 111u, 0u, 30u, 30u, 1111u},
        {"lower", "hydrology/animations/lower-0000000000000016.mhwa",
         22u, 222u, 0u, 30u, 30u, 2222u},
    };
    manifest.handoff_animations = {
        {"pool-one",
         "hydrology/animations/handoffs/pool-one-0000000000000021.mhwa",
         33u, 111u, 222u, 30u, 30u, 3333u},
    };
    return manifest;
}

hydrology::WaterMeshAnimationArtifact animation_fixture(
    const hydrology::HydrologyWaterAnimationReference& reference) {
    hydrology::WaterMeshAnimation animation{};
    animation.frames_per_second = 30u;
    animation.phase_offset_frames = 15u;
    animation.duration_seconds = 1.0f;
    animation.frames.resize(30u);
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        auto& mesh = animation.frames[frame];
        const float offset = static_cast<float>(frame) * 0.001f;
        mesh.positions = {offset, 0.0f, 0.0f,
                          1.0f + offset, 0.1f, 0.0f,
                          offset, 0.5f, 1.0f};
        mesh.normals = {0,1,0, 0,1,0, 0,1,0};
        mesh.indices = {0u, 1u, 2u};
        mesh.material = 4u;
        mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
    }
    hydrology::WaterMeshAnimationArtifact artifact{};
    gpu_meshing::Error error{};
    CHECK(hydrology::pack_water_mesh_animation_artifact(
              {reference.id, reference.semantic_key,
               reference.source_primary_payload_digest,
               reference.source_secondary_payload_digest, 0.2f,
               {{0.0f, 0.0f, 0.0f}, 0.2f, 1u}},
              animation, artifact, error), error.message.c_str());
    return artifact;
}

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>()};
}

bool save_field_pair(const std::filesystem::path& root,
                     gpu_meshing::Error& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) return false;
    const auto manifest = fixture_manifest();
    return hydrology::save_hydrology_field_product_atomic(
               root / manifest.field_products[0].relative_path,
               fixture_runtime_product(), error) &&
           hydrology::save_hydrology_field_product_atomic(
               root / manifest.field_products[1].relative_path,
               fixture_presentation_product(), error);
}

#ifdef _WIN32
bool create_junction(const std::filesystem::path& link,
                     const std::filesystem::path& target) {
    struct JunctionReparseBuffer {
        DWORD tag;
        USHORT data_length;
        USHORT reserved;
        USHORT substitute_offset;
        USHORT substitute_length;
        USHORT print_offset;
        USHORT print_length;
        WCHAR path[1];
    };
    std::error_code error;
    std::filesystem::create_directories(link, error);
    if (error) return false;
    const std::wstring substitute = L"\\??\\" +
        std::filesystem::absolute(target).native();
    const std::wstring print = std::filesystem::absolute(target).native();
    const std::size_t path_bytes =
        (substitute.size() + print.size() + 2u) * sizeof(wchar_t);
    std::vector<std::uint8_t> storage(
        offsetof(JunctionReparseBuffer, path) + path_bytes,
        0u);
    auto* const reparse = reinterpret_cast<JunctionReparseBuffer*>(
        storage.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->substitute_offset = 0u;
    reparse->substitute_length =
        static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    reparse->print_offset =
        static_cast<USHORT>((substitute.size() + 1u) * sizeof(wchar_t));
    reparse->print_length =
        static_cast<USHORT>(print.size() * sizeof(wchar_t));
    std::memcpy(reparse->path,
                substitute.c_str(), (substitute.size() + 1u) * sizeof(wchar_t));
    std::memcpy(
        reinterpret_cast<std::uint8_t*>(reparse->path) + reparse->print_offset,
        print.c_str(), (print.size() + 1u) * sizeof(wchar_t));
    reparse->data_length = static_cast<USHORT>(storage.size() - 8u);
    const HANDLE directory = CreateFileW(
        link.c_str(), GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0u;
    const bool result = DeviceIoControl(
        directory, FSCTL_SET_REPARSE_POINT, reparse,
        static_cast<DWORD>(storage.size()), nullptr, 0u, &returned, nullptr) != 0;
    CloseHandle(directory);
    if (!result) std::filesystem::remove(link, error);
    return result;
}
#endif

struct ConcurrentFieldMutation {
    std::filesystem::path target;
    std::filesystem::path replacement;
    bool write_succeeded = false;
    bool replace_succeeded = false;
};

struct DirectoryNamespaceSwap {
    std::filesystem::path active;
    std::filesystem::path held;
    std::filesystem::path replacement;
    bool invoked = false;
    bool rename_succeeded = false;
    bool link_succeeded = false;
};

void attempt_directory_namespace_swap(
    const std::filesystem::path& opened, void* opaque) noexcept {
    auto& context = *static_cast<DirectoryNamespaceSwap*>(opaque);
    if (context.invoked || opened.parent_path() != context.active) return;
    context.invoked = true;
#ifdef _WIN32
    context.rename_succeeded = MoveFileExW(
        context.active.c_str(), context.held.c_str(), 0u) != 0;
#else
    context.rename_succeeded =
        std::rename(context.active.c_str(), context.held.c_str()) == 0;
#endif
    if (!context.rename_succeeded) return;
    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        context.replacement, context.active, link_error);
    context.link_succeeded = !link_error;
}

void restore_directory_namespace(DirectoryNamespaceSwap& context) {
    if (!context.rename_succeeded) return;
    std::error_code error;
    if (context.link_succeeded)
        std::filesystem::remove(context.active, error);
    error.clear();
    std::filesystem::rename(context.held, context.active, error);
}

bool contains_temporary(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename().string().find(".tmp-") !=
            std::string::npos)
            return true;
    return false;
}

struct ManifestTemporaryReparse {
    std::filesystem::path outside_target;
    std::filesystem::path created_leaf;
    bool attempted = false;
    bool created = false;
};

struct ManifestCommitInterference {
    std::filesystem::path replacement;
    bool before_invoked = false;
    bool write_succeeded = false;
    bool delete_succeeded = false;
    bool replace_succeeded = false;
};

void attempt_manifest_commit_interference(
    hydrology::HydrologyManifestPublicationTestStage stage,
    const std::filesystem::path& path,
    void* opaque) noexcept {
    auto& context = *static_cast<ManifestCommitInterference*>(opaque);
    if (stage != hydrology::HydrologyManifestPublicationTestStage::BeforeCommit)
        return;
    context.before_invoked = true;
#ifdef _WIN32
    const HANDLE writer = CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    context.write_succeeded = writer != INVALID_HANDLE_VALUE;
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    context.delete_succeeded = DeleteFileW(path.c_str()) != 0;
    context.replace_succeeded = MoveFileExW(
        context.replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    (void)path;
#endif
}

struct ManifestAfterCommitSwap {
    DirectoryNamespaceSwap swap;
    bool invoked = false;
};

void swap_manifest_parent_after_commit(
    hydrology::HydrologyManifestPublicationTestStage stage,
    const std::filesystem::path& path,
    void* opaque) noexcept {
    auto& context = *static_cast<ManifestAfterCommitSwap*>(opaque);
    if (stage != hydrology::HydrologyManifestPublicationTestStage::AfterCommit)
        return;
    context.invoked = true;
    attempt_directory_namespace_swap(path, &context.swap);
}

void inject_manifest_temporary_reparse(
    const std::filesystem::path& opened, void* opaque) noexcept {
    auto& context = *static_cast<ManifestTemporaryReparse*>(opaque);
#ifdef _WIN32
    const bool temporary = opened.native().find(L".tmp-") !=
        std::wstring::npos;
#else
    const bool temporary = opened.native().find(".tmp-") !=
        std::string::npos;
#endif
    if (context.attempted ||
        !temporary)
        return;
    context.attempted = true;
    context.created_leaf = opened;
    std::error_code error;
    std::filesystem::create_symlink(
        context.outside_target, opened, error);
    context.created = !error;
}

void attempt_concurrent_field_mutation(
    const std::filesystem::path& opened, void* opaque) noexcept {
    auto& context = *static_cast<ConcurrentFieldMutation*>(opaque);
    if (opened != context.target) return;
    {
        std::ofstream stream(opened, std::ios::binary | std::ios::app);
        if (stream) {
            const char byte = 0;
            stream.write(&byte, 1);
            context.write_succeeded = static_cast<bool>(stream);
        }
    }
#ifdef _WIN32
    context.replace_succeeded = MoveFileExW(
        context.replacement.c_str(), opened.c_str(),
        MOVEFILE_REPLACE_EXISTING) != 0;
#else
    context.replace_succeeded =
        std::rename(context.replacement.c_str(), opened.c_str()) == 0;
#endif
}

void check_manifest_preserved(
    const hydrology::HydrologyNetworkArtifact& artifact,
    const std::vector<std::uint8_t>& expected, const char* message) {
    std::vector<std::uint8_t> actual;
    gpu_meshing::Error error{};
    CHECK(hydrology::serialize_network_artifact(artifact, actual, error) &&
              actual == expected,
          message);
}

void test_manifest_round_trip_is_canonical_and_transactional() {
    auto manifest = fixture_manifest();
    std::vector<std::uint8_t> bytes;
    gpu_meshing::Error error{};
    CHECK(hydrology::serialize_network_artifact(manifest, bytes, error),
          error.message.c_str());
    std::vector<std::uint8_t> reversed_bytes;
    CHECK(hydrology::serialize_network_artifact(
              fixture_manifest(true), reversed_bytes, error) &&
              reversed_bytes == bytes,
          "manifest bytes ignore section and dependency declaration order");

    hydrology::HydrologyNetworkArtifact reopened{};
    CHECK(hydrology::deserialize_network_artifact(bytes, reopened, error),
          error.message.c_str());
    CHECK(reopened.state == hydrology::HydrologyNetworkState::Ready &&
              reopened.sections.size() == 2u &&
              reopened.sections[0].id == "lower" &&
              reopened.sections[1].id == "upper" &&
              reopened.topological_order ==
                  std::vector<std::string>({"upper", "lower"}) &&
              reopened.runtime_field_digest == manifest.runtime_field_digest &&
              reopened.presentation_field_digest ==
                  manifest.presentation_field_digest &&
              reopened.field_products.size() == 2u &&
              reopened.payload_digest != 0u,
          "ready manifest identity, field digests, and package closure round-trip");

    auto invalid = manifest;
    invalid.sections.push_back(invalid.sections.front());
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "duplicate section references are rejected");
    invalid = manifest;
    invalid.sections[1].dependencies = {"missing"};
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "missing dependencies are rejected");
    invalid = manifest;
    invalid.sections[0].relative_path = "../outside.mhyd";
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "artifact references must remain relative to the network cache");
    invalid = manifest;
    invalid.field_products[0].relative_path =
        "hydrology/fields/runtime.mhydfield";
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "a non-content-addressed field path is rejected");
    invalid = manifest;
    invalid.field_products[0].relative_path = "../runtime.mhydfield";
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "a parent-traversing field path is rejected");
    invalid = manifest;
    invalid.field_products[0].relative_path =
        std::filesystem::absolute("runtime.mhydfield").generic_string();
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "an absolute field path is rejected");
    invalid = manifest;
    invalid.field_products.pop_back();
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "a Ready package missing the presentation field payload is rejected");
    invalid = manifest;
    invalid.field_products[0].payload_digest++;
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "a Ready package carrying a stale runtime field digest is rejected");
    invalid = manifest;
    invalid.field_products.push_back({
        hydrology::HydrologyFieldProductKind::Runtime,
        "fields/stale-runtime.mhydfield", manifest.runtime_field_digest});
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "a Ready package carrying an extra field reference is rejected");
    invalid = manifest;
    invalid.field_products[1].relative_path =
        invalid.field_products[0].relative_path;
    CHECK(!hydrology::serialize_network_artifact(invalid, reversed_bytes, error),
          "duplicate field paths are rejected");

    const auto sentinel = bytes;
    auto corrupt = bytes;
    corrupt.back() ^= 0x80u;
    CHECK(!hydrology::deserialize_network_artifact(corrupt, reopened, error),
          "manifest payload corruption is rejected");
    check_manifest_preserved(
        reopened, sentinel,
        "manifest corruption preserves the caller's prior valid object");
    corrupt = bytes;
    corrupt.resize(corrupt.size() - 1u);
    CHECK(!hydrology::deserialize_network_artifact(corrupt, reopened, error),
          "truncated manifests are rejected");
    check_manifest_preserved(
        reopened, sentinel,
        "manifest truncation preserves the caller's prior valid object");
}

void test_animation_manifest_extension_and_legacy_bytes() {
    gpu_meshing::Error error{};
    std::vector<std::uint8_t> legacy;
    CHECK(hydrology::serialize_network_artifact(
              fixture_manifest(), legacy, error), error.message.c_str());
    CHECK(legacy.size() > 12u && legacy[8] == 2u && legacy[9] == 0u &&
              legacy[10] == 0u && legacy[11] == 0u,
          "animation-disabled manifests retain the exact version-two path");

    auto animated = animated_fixture_manifest();
    std::vector<std::uint8_t> bytes;
    CHECK(hydrology::serialize_network_artifact(animated, bytes, error),
          error.message.c_str());
    CHECK(bytes.size() > 12u && bytes[8] == 3u && bytes != legacy,
          "animation references select a digest-participating version-three manifest");
    hydrology::HydrologyNetworkArtifact loaded{};
    CHECK(hydrology::deserialize_network_artifact(bytes, loaded, error),
          error.message.c_str());
    CHECK(loaded.section_animations.size() == 2u &&
              loaded.section_animations[0].id == "upper" &&
              loaded.section_animations[1].id == "lower" &&
              loaded.handoff_animations.size() == 1u &&
              loaded.handoff_animations[0].source_primary_payload_digest == 111u &&
              loaded.handoff_animations[0].source_secondary_payload_digest == 222u,
          "section animations follow topology and handoff sources round-trip");

    auto changed = animated;
    changed.section_animations[0].payload_digest++;
    std::vector<std::uint8_t> changed_bytes;
    CHECK(hydrology::serialize_network_artifact(
              changed, changed_bytes, error) && changed_bytes != bytes,
          "animation payload references participate in manifest digesting");
    auto invalid = animated;
    std::swap(invalid.section_animations[0], invalid.section_animations[1]);
    CHECK(!hydrology::serialize_network_artifact(
              invalid, changed_bytes, error),
          "section animation references must remain in topological order");
    invalid = animated;
    invalid.handoff_animations.clear();
    CHECK(!hydrology::serialize_network_artifact(
              invalid, changed_bytes, error),
          "a ready animated manifest requires every handoff animation");
    invalid = animated;
    invalid.section_animations[0].relative_path = "../upper.mhwa";
    CHECK(!hydrology::serialize_network_artifact(
              invalid, changed_bytes, error),
          "animation references remain confined to the network cache");
}

void test_manifest_content_address_tracks_product_generation() {
    const auto original = animated_fixture_manifest();
    auto reordered = original;
    std::reverse(reordered.sections.begin(), reordered.sections.end());
    auto changed_handoff = original;
    changed_handoff.handoffs.front().semantic_key += 1u;
    changed_handoff.handoffs.front().payload_digest += 1u;

    const std::uint64_t original_digest =
        hydrology::hydrology_network_artifact_content_digest(original);
    CHECK(original_digest != 0u &&
              hydrology::hydrology_network_artifact_content_digest(
                  reordered) == original_digest,
          "manifest content addressing follows canonical order");
    CHECK(hydrology::hydrology_network_artifact_content_digest(
              changed_handoff) != original_digest,
          "a rebuilt handoff generation receives a distinct immutable manifest address");
}

void test_ready_animation_package_rejects_missing_or_corrupt_payload() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("matter-hydrology-animation-package-" + std::to_string(stamp));
    const auto manifest_path = root / "hydrology" / "network.mhyn";
    auto manifest = animated_fixture_manifest();
    gpu_meshing::Error error{};
    CHECK(save_field_pair(root, error), error.message.c_str());
    for (auto& reference : manifest.section_animations) {
        auto artifact = animation_fixture(reference);
        CHECK(hydrology::save_water_mesh_animation_artifact_immutable(
                  root / reference.relative_path, artifact, error),
              error.message.c_str());
        CHECK(hydrology::load_water_mesh_animation_artifact(
                  root / reference.relative_path, artifact, error),
              error.message.c_str());
        reference.payload_digest = artifact.payload_digest;
    }
    for (auto& reference : manifest.handoff_animations) {
        auto artifact = animation_fixture(reference);
        CHECK(hydrology::save_water_mesh_animation_artifact_immutable(
                  root / reference.relative_path, artifact, error),
              error.message.c_str());
        CHECK(hydrology::load_water_mesh_animation_artifact(
                  root / reference.relative_path, artifact, error),
              error.message.c_str());
        reference.payload_digest = artifact.payload_digest;
    }
    CHECK(hydrology::save_network_artifact_atomic(
              manifest_path, manifest, error), error.message.c_str());
    hydrology::HydrologyNetworkArtifact loaded{};
    CHECK(hydrology::load_network_artifact_validated(
              manifest_path, manifest.network_key,
              manifest.terrain_revision, loaded, error),
          error.message.c_str());
    const auto preserved = loaded;
    const auto corrupt_path =
        root / manifest.section_animations.front().relative_path;
    auto corrupt = read_bytes(corrupt_path);
    corrupt.back() ^= 0x80u;
    CHECK(write_bytes(corrupt_path, corrupt),
          "animation corruption fixture was written");
    CHECK(!hydrology::load_network_artifact_validated(
              manifest_path, manifest.network_key,
              manifest.terrain_revision, loaded, error) &&
              loaded.payload_digest == preserved.payload_digest,
          "a corrupt animation prevents Ready activation transactionally");
    std::filesystem::remove_all(root);
}

void test_typed_field_wire_format_and_ready_package_closure() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("matter-hydrology-network-" + std::to_string(stamp));
    const auto path = root / "river.mhydnet";
    const auto manifest = fixture_manifest();
    const auto runtime_path = root / manifest.field_products[0].relative_path;
    const auto presentation_path =
        root / manifest.field_products[1].relative_path;
    gpu_meshing::Error error{};
    CHECK(save_field_pair(root, error), error.message.c_str());

    std::vector<std::uint8_t> runtime_bytes;
    CHECK(hydrology::serialize_hydrology_field_product(
              fixture_runtime_product(), runtime_bytes, error),
          error.message.c_str());
    hydrology::HydrologyFieldProduct decoded =
        fixture_presentation_product();
    CHECK(hydrology::deserialize_hydrology_field_product(
              runtime_bytes, decoded, error) &&
              decoded.kind == hydrology::HydrologyFieldProductKind::Runtime &&
              decoded.gameplay == fixture_runtime_product().gameplay &&
              decoded.presentation.empty(),
          "the bounded field wire format round-trips exact typed runtime bytes");
    const auto decoded_sentinel = decoded;
    auto truncated = runtime_bytes;
    truncated.pop_back();
    CHECK(!hydrology::deserialize_hydrology_field_product(
              truncated, decoded, error) &&
              decoded.gameplay == decoded_sentinel.gameplay &&
              decoded.payload_digest == decoded_sentinel.payload_digest,
          "field-product truncation is transactional");

    CHECK(hydrology::save_network_artifact_atomic(path, manifest, error),
          error.message.c_str());
    hydrology::HydrologyNetworkArtifact loaded{};
    CHECK(hydrology::load_network_artifact_validated(
              path, 101u, 202u, loaded, error) &&
              loaded.state == hydrology::HydrologyNetworkState::Ready,
          error.message.c_str());

    const auto replacement_path =
        std::filesystem::path(runtime_path.string() + ".replacement");
    CHECK(write_bytes(replacement_path, read_bytes(presentation_path)),
          "concurrent replacement fixture was written");
    ConcurrentFieldMutation mutation{runtime_path, replacement_path};
    hydrology::set_hydrology_field_validation_test_hook(
        attempt_concurrent_field_mutation, &mutation);
    const bool concurrent_load = hydrology::load_network_artifact_validated(
        path, 101u, 202u, loaded, error);
    hydrology::set_hydrology_field_validation_test_hook(nullptr, nullptr);
#ifdef _WIN32
    CHECK(concurrent_load && !mutation.write_succeeded &&
              !mutation.replace_succeeded,
          "an open field validation handle prohibits concurrent write and replacement");
#else
    CHECK(!concurrent_load &&
              (mutation.write_succeeded || mutation.replace_succeeded),
          "a concurrent POSIX mutation is detected before Ready validation succeeds");
#endif
    std::filesystem::remove(replacement_path);
    std::vector<std::uint8_t> loaded_sentinel;
    CHECK(hydrology::serialize_network_artifact(
              loaded, loaded_sentinel, error), error.message.c_str());
    const auto rejected_load_preserves = [&](const char* message) {
        CHECK(!hydrology::load_network_artifact_validated(
                  path, 101u, 202u, loaded, error), message);
        check_manifest_preserved(
            loaded, loaded_sentinel,
            "failed Ready package loads preserve the caller's prior manifest");
    };

    const auto valid_manifest_bytes = read_bytes(path);
    auto rejected_manifest_bytes = valid_manifest_bytes;
    rejected_manifest_bytes[0] ^= 0x40u;
    CHECK(write_bytes(path, rejected_manifest_bytes),
          "corrupt manifest fixture was written");
    rejected_load_preserves("a corrupt Ready manifest file is rejected");
    CHECK(write_bytes(path, valid_manifest_bytes),
          "valid manifest fixture was restored");
    rejected_manifest_bytes = valid_manifest_bytes;
    rejected_manifest_bytes.pop_back();
    CHECK(write_bytes(path, rejected_manifest_bytes),
          "truncated manifest fixture was written");
    rejected_load_preserves("a truncated Ready manifest file is rejected");
    CHECK(write_bytes(path, valid_manifest_bytes),
          "valid manifest fixture was restored");
    rejected_manifest_bytes = valid_manifest_bytes;
    rejected_manifest_bytes.back() ^= 0x80u;
    CHECK(write_bytes(path, rejected_manifest_bytes),
          "digest-invalid manifest fixture was written");
    rejected_load_preserves("a digest-invalid Ready manifest file is rejected");
    CHECK(write_bytes(path, valid_manifest_bytes),
          "valid manifest fixture was restored");

    std::filesystem::remove(runtime_path);
    rejected_load_preserves("a Ready manifest with a missing runtime file is rejected");
    CHECK(save_field_pair(root, error), error.message.c_str());

    auto presentation_bytes = read_bytes(presentation_path);
    presentation_bytes.back() ^= 0x80u;
    CHECK(write_bytes(presentation_path, presentation_bytes),
          "corrupt presentation fixture was written");
    rejected_load_preserves("a Ready manifest with a corrupt field file is rejected");
    std::filesystem::remove(presentation_path);
    CHECK(save_field_pair(root, error), error.message.c_str());

    auto grown_runtime = read_bytes(runtime_path);
    grown_runtime.push_back(0u);
    CHECK(write_bytes(runtime_path, grown_runtime),
          "grown runtime fixture was written");
    rejected_load_preserves(
        "a Ready manifest with trailing field bytes is rejected");
    std::filesystem::remove(runtime_path);
    CHECK(save_field_pair(root, error), error.message.c_str());

    auto truncated_runtime = read_bytes(runtime_path);
    truncated_runtime.pop_back();
    CHECK(write_bytes(runtime_path, truncated_runtime),
          "truncated runtime fixture was written");
    rejected_load_preserves("a Ready manifest with a truncated field file is rejected");
    std::filesystem::remove(runtime_path);
    CHECK(save_field_pair(root, error), error.message.c_str());

    const auto valid_runtime_bytes = read_bytes(runtime_path);
    const auto valid_presentation_bytes = read_bytes(presentation_path);
    CHECK(write_bytes(runtime_path, valid_presentation_bytes) &&
              write_bytes(presentation_path, valid_runtime_bytes),
          "swapped typed field fixtures were written");
    rejected_load_preserves("swapped runtime/presentation field types are rejected");
    std::filesystem::remove(runtime_path);
    std::filesystem::remove(presentation_path);
    CHECK(save_field_pair(root, error), error.message.c_str());

    auto corrupt_canonical = read_bytes(runtime_path);
    corrupt_canonical.back() ^= 0x20u;
    CHECK(write_bytes(runtime_path, corrupt_canonical),
          "canonical immutable corruption fixture was written");
    CHECK(!hydrology::save_hydrology_field_product_atomic(
              runtime_path, fixture_runtime_product(), error) &&
              read_bytes(runtime_path) == corrupt_canonical,
          "an immutable canonical leaf with changed bytes is rejected without replacement");
    std::filesystem::remove(runtime_path);
    CHECK(save_field_pair(root, error), error.message.c_str());

    CHECK(!hydrology::load_network_artifact_validated(
              path, 999u, 202u, loaded, error),
          "a stale Ready network key is rejected");
    check_manifest_preserved(
        loaded, loaded_sentinel,
        "stale identity rejection preserves the caller's prior manifest");

    for (const auto& entry : std::filesystem::directory_iterator(
             root / "hydrology" / "fields"))
        CHECK(entry.path().filename().string().find(".tmp-") ==
                  std::string::npos,
              "atomic field publication leaves no temporary files");

    auto incomplete = manifest;
    incomplete.state = hydrology::HydrologyNetworkState::Incomplete;
    incomplete.sections.clear();
    incomplete.handoffs.clear();
    incomplete.topological_order.clear();
    incomplete.runtime_field_digest = 0u;
    incomplete.presentation_field_digest = 0u;
    incomplete.field_products.clear();
    const auto incomplete_path = root / "river-incomplete.mhydnet";
    CHECK(hydrology::save_network_artifact_atomic(
              incomplete_path, incomplete, error),
          error.message.c_str());
    CHECK(!hydrology::load_network_artifact_validated(
              incomplete_path, 101u, 202u, loaded, error),
          "an incomplete manifest is persisted for diagnostics but is never ready");
    check_manifest_preserved(
        loaded, loaded_sentinel,
        "incomplete Ready loads preserve the caller's prior manifest");
    std::filesystem::remove_all(root);
}


void test_ready_package_rejects_reparse_escape_when_supported() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("matter-hydrology-reparse-" + std::to_string(stamp));
    const auto outside = std::filesystem::temp_directory_path() /
        ("matter-hydrology-outside-" + std::to_string(stamp));
    const auto path = root / "river.mhydnet";
    gpu_meshing::Error error{};
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    const auto manifest = fixture_manifest();
    std::vector<std::uint8_t> bytes;
    CHECK(hydrology::serialize_network_artifact(manifest, bytes, error) &&
              write_bytes(path, bytes),
          "reparse Ready manifest fixture was written");
    bool linked = false;
#ifdef _WIN32
    linked = create_junction(root / "hydrology", outside);
#else
    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        outside, root / "hydrology", link_error);
    linked = !link_error;
#endif
    if (linked) {
        hydrology::HydrologyNetworkArtifact loaded{};
        CHECK(!save_field_pair(root, error),
              "field publication cannot traverse a symlink or reparse directory");
        CHECK(!std::filesystem::exists(outside / "fields"),
              "reparse rejection creates no directory or file outside the cache root");
        CHECK(!hydrology::load_network_artifact_validated(
                  path, 101u, 202u, loaded, error),
              "a Ready package cannot traverse a symlink or reparse directory");
    } else {
        std::printf("SKIP: platform could not create hydrology reparse fixture\n");
    }

    std::error_code cleanup_error;
    std::filesystem::remove(root / "hydrology", cleanup_error);
    cleanup_error.clear();
    CHECK(save_field_pair(root, error), error.message.c_str());
    const auto runtime_path =
        root / manifest.field_products[0].relative_path;
    const auto runtime_bytes = read_bytes(runtime_path);
    const auto outside_leaf = outside / "runtime-external.mhydfield";
    CHECK(write_bytes(outside_leaf, runtime_bytes),
          "outside leaf reparse target fixture was written");
    std::filesystem::remove(runtime_path, cleanup_error);
    cleanup_error.clear();
    std::filesystem::create_symlink(outside_leaf, runtime_path, cleanup_error);
    if (!cleanup_error) {
        hydrology::HydrologyNetworkArtifact loaded{};
        CHECK(!hydrology::save_hydrology_field_product_atomic(
                  runtime_path, fixture_runtime_product(), error) &&
                  read_bytes(outside_leaf) == runtime_bytes,
              "field publication rejects a canonical leaf reparse without changing its target");
        CHECK(!hydrology::load_network_artifact_validated(
                  path, 101u, 202u, loaded, error),
              "Ready validation rejects a canonical leaf reparse");
    } else {
        std::printf("SKIP: platform could not create hydrology leaf reparse fixture\n");
    }
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

void test_allocation_failure_closes_native_resources_transactionally() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
        ("matter-hydrology-allocation-" + std::to_string(stamp));
    const auto manifest = fixture_manifest();
    const auto product = fixture_runtime_product();
    gpu_meshing::Error error{};
    for (const auto point : {
             hydrology::HydrologyFieldIoFailurePoint::AfterRootHandle,
             hydrology::HydrologyFieldIoFailurePoint::AfterDirectoryHandle,
             hydrology::HydrologyFieldIoFailurePoint::AfterFileHandle}) {
        const auto root = base / std::to_string(static_cast<unsigned>(point));
        std::filesystem::create_directories(root);
        const auto field = root / manifest.field_products[0].relative_path;
        hydrology::set_hydrology_field_io_failure_for_test(point);
        const bool saved = hydrology::save_hydrology_field_product_atomic(
            field, product, error);
        const bool allocation_error_populated =
            error.code == gpu_meshing::ErrorCode::ArtifactFailure;
        hydrology::set_hydrology_field_io_failure_for_test(
            hydrology::HydrologyFieldIoFailurePoint::None);
        bool temporary_exists = false;
        const auto field_directory = root / "hydrology" / "fields";
        if (std::filesystem::exists(field_directory)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(field_directory))
                temporary_exists = temporary_exists ||
                    entry.path().filename().string().find(".tmp-") !=
                        std::string::npos;
        }
        const auto network_path = root / "river.mhydnet";
        const bool ready_saved = hydrology::save_network_artifact_atomic(
            network_path, manifest, error);
        CHECK(!saved && !temporary_exists &&
                  !std::filesystem::exists(field) && !ready_saved &&
                  !std::filesystem::exists(network_path) &&
                  allocation_error_populated,
               "injected allocation failure publishes no mutable canonical field");
        std::error_code remove_error;
        std::filesystem::remove_all(root, remove_error);
        CHECK(!remove_error,
              "injected allocation failure closes every acquired native resource");
    }

    const auto load_root = base / "load";
    CHECK(save_field_pair(load_root, error), error.message.c_str());
    const auto runtime_path = load_root / manifest.field_products[0].relative_path;
    hydrology::HydrologyFieldProduct sentinel = fixture_presentation_product();
    std::vector<std::uint8_t> sentinel_bytes;
    CHECK(hydrology::serialize_hydrology_field_product(
              sentinel, sentinel_bytes, error), error.message.c_str());
    hydrology::set_hydrology_field_io_failure_for_test(
        hydrology::HydrologyFieldIoFailurePoint::AfterFileHandle);
    const bool loaded = hydrology::load_hydrology_field_product_validated(
        runtime_path, hydrology::HydrologyFieldProductKind::Runtime,
        product.payload_digest, sentinel, error);
    hydrology::set_hydrology_field_io_failure_for_test(
        hydrology::HydrologyFieldIoFailurePoint::None);
    std::vector<std::uint8_t> preserved_bytes;
    CHECK(hydrology::serialize_hydrology_field_product(
              sentinel, preserved_bytes, error), error.message.c_str());
    CHECK(!loaded && preserved_bytes == sentinel_bytes,
           "allocation failure after file acquisition preserves the caller output");
    std::error_code remove_error;
    std::filesystem::remove_all(base, remove_error);
    CHECK(!remove_error,
          "allocation-failed load closes its file handle for immediate cleanup");
}

void test_file_identity_requires_one_stable_native_object() {
    const hydrology::HydrologyFileIdentity original{11u, 22u};
    CHECK(hydrology::hydrology_file_identity_stable(
              original, original, original),
          "one native object remains a stable publication identity");
    CHECK(!hydrology::hydrology_file_identity_stable(
              original, {11u, 23u}, original) &&
              !hydrology::hydrology_file_identity_stable(
                  original, original, {12u, 22u}),
          "a namespace replacement before or after IO invalidates publication identity");

    char path[32]{};
    CHECK(hydrology::format_hydrology_proc_fd_path(
              17, path, sizeof(path)) &&
              std::string(path) == "/proc/self/fd/17",
          "a retained descriptor formats the documented unprivileged procfs source path");
    char exact[17]{};
    CHECK(hydrology::format_hydrology_proc_fd_path(
              17, exact, sizeof(exact)) &&
              std::string(exact) == "/proc/self/fd/17",
          "procfs descriptor formatting accepts the exact terminated buffer size");
    char truncated[16] = {'x'};
    CHECK(!hydrology::format_hydrology_proc_fd_path(
              17, truncated, sizeof(truncated)) && truncated[0] == '\0' &&
              !hydrology::format_hydrology_proc_fd_path(
                  -1, path, sizeof(path)),
          "procfs descriptor formatting rejects truncation and invalid descriptors");
}

void test_posix_publication_source_contract_uses_one_unprivileged_helper() {
    const auto source_path = std::filesystem::path(__FILE__).parent_path()
        .parent_path() / "src" / "hydrology" /
        "hydrology_network_artifact.cpp";
    const auto bytes = read_bytes(source_path);
    const std::string source(bytes.begin(), bytes.end());
    const std::string helper = "publish_posix_retained_fd_create_new(";
    std::size_t count = 0u;
    for (std::size_t position = source.find(helper);
         position != std::string::npos;
         position = source.find(helper, position + helper.size()))
        ++count;
    CHECK(!source.empty() && count == 3u &&
              source.find("AT_SYMLINK_FOLLOW") != std::string::npos &&
              source.find("AT_EMPTY_PATH") == std::string::npos,
          "field and manifest POSIX publication share the unprivileged procfs retained-fd helper");
}

void test_confined_field_open_uses_the_held_directory_identity() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
        ("matter-hydrology-field-namespace-" + std::to_string(stamp));
    const auto root = base / "cache";
    const auto outside = base / "outside";
    const auto manifest_path = root / "river.mhydnet";
    gpu_meshing::Error error{};
    std::filesystem::create_directories(outside);
    CHECK(save_field_pair(root, error), error.message.c_str());
    const auto manifest = fixture_manifest();
    CHECK(hydrology::save_network_artifact_atomic(
              manifest_path, manifest, error), error.message.c_str());

    DirectoryNamespaceSwap swap{
        root / "hydrology" / "fields",
        root / "hydrology" / "fields-held", outside};
    hydrology::set_hydrology_namespace_validation_test_hook(
        attempt_directory_namespace_swap, &swap);
    hydrology::HydrologyNetworkArtifact loaded{};
    const bool accepted = hydrology::load_network_artifact_validated(
        manifest_path, 101u, 202u, loaded, error);
    hydrology::set_hydrology_namespace_validation_test_hook(nullptr, nullptr);
#ifdef _WIN32
    CHECK(accepted && swap.invoked && !swap.rename_succeeded,
          "held Windows field directory prevents a namespace swap before relative leaf open");
#else
    CHECK(!accepted && swap.invoked && swap.rename_succeeded,
          "relative POSIX field open remains anchored and fails closed after a namespace swap");
#endif
    restore_directory_namespace(swap);

    DirectoryNamespaceSwap direct_swap{
        root / "hydrology" / "fields",
        root / "hydrology" / "fields-held-direct", outside};
    hydrology::set_hydrology_namespace_validation_test_hook(
        attempt_directory_namespace_swap, &direct_swap);
    hydrology::HydrologyFieldProduct direct_product =
        fixture_presentation_product();
    const bool direct_accepted =
        hydrology::load_hydrology_field_product_validated(
            root / manifest.field_products[0].relative_path,
            hydrology::HydrologyFieldProductKind::Runtime,
            fixture_runtime_product().payload_digest, direct_product, error);
    hydrology::set_hydrology_namespace_validation_test_hook(nullptr, nullptr);
#ifdef _WIN32
    CHECK(direct_accepted && direct_swap.invoked &&
              !direct_swap.rename_succeeded,
          "direct Windows field load opens relative to and locks its trusted parent");
#else
    CHECK(!direct_accepted && direct_swap.invoked &&
              direct_swap.rename_succeeded,
          "direct POSIX field load rejects a changed trusted-parent identity");
#endif
    restore_directory_namespace(direct_swap);
    std::filesystem::remove_all(base);
}

void test_manifest_publication_is_confined_and_transactional() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
        ("matter-hydrology-manifest-io-" + std::to_string(stamp));
    const auto root = base / "cache";
    const auto path = root / "river.mhydnet";
    auto original = fixture_manifest();
    auto replacement = original;
    replacement.bounds_m.maximum.x += 1.0f;
    gpu_meshing::Error error{};
    CHECK(save_field_pair(root, error), error.message.c_str());

    std::vector<std::uint8_t> original_bytes;
    CHECK(hydrology::serialize_network_artifact(
              original, original_bytes, error), error.message.c_str());
    CHECK(hydrology::save_network_artifact_atomic(path, original, error) &&
              hydrology::save_network_artifact_atomic(path, original, error),
          "an identical existing immutable manifest is accepted");
    CHECK(!hydrology::save_network_artifact_atomic(path, replacement, error) &&
              read_bytes(path) == original_bytes,
          "a differing immutable manifest collision is rejected without replacement");

    for (const auto point : {
             hydrology::HydrologyFieldIoFailurePoint::AfterManifestParentHandle,
             hydrology::HydrologyFieldIoFailurePoint::AfterManifestFileHandle,
             hydrology::HydrologyFieldIoFailurePoint::BeforeManifestRename}) {
        CHECK(hydrology::save_network_artifact_atomic(path, original, error),
              error.message.c_str());
        const auto before = read_bytes(path);
        hydrology::set_hydrology_field_io_failure_for_test(point);
        const bool saved = hydrology::save_network_artifact_atomic(
            path, replacement, error);
        hydrology::set_hydrology_field_io_failure_for_test(
            hydrology::HydrologyFieldIoFailurePoint::None);
        CHECK(!saved && read_bytes(path) == before &&
                  !contains_temporary(root),
              "manifest allocation failure preserves the old manifest and leaves no temporary");
    }

    CHECK(hydrology::save_network_artifact_atomic(path, original, error),
          error.message.c_str());
    const auto before_temp_attack = read_bytes(path);
    const auto temp_attack_target = base / "temporary-external.mhydnet";
    const std::vector<std::uint8_t> temp_attack_bytes = {9u, 8u, 7u};
    CHECK(write_bytes(temp_attack_target, temp_attack_bytes),
          "manifest temporary reparse target fixture was written");
    ManifestTemporaryReparse temp_attack{temp_attack_target};
    hydrology::set_hydrology_namespace_validation_test_hook(
        inject_manifest_temporary_reparse, &temp_attack);
    const bool temp_attack_saved = hydrology::save_network_artifact_atomic(
        path, replacement, error);
    hydrology::set_hydrology_namespace_validation_test_hook(nullptr, nullptr);
    if (temp_attack.created) {
        CHECK(!temp_attack_saved && read_bytes(path) == before_temp_attack &&
                  read_bytes(temp_attack_target) == temp_attack_bytes &&
                  std::filesystem::is_symlink(std::filesystem::symlink_status(
                      temp_attack.created_leaf)),
              "manifest temporary reparse is rejected without deleting the replacement or old manifest");
        std::error_code remove_error;
        std::filesystem::remove(temp_attack.created_leaf, remove_error);
    } else {
        std::printf("SKIP: platform could not create manifest temporary reparse fixture\n");
    }

    const auto outside = base / "outside";
    const auto linked_parent = base / "linked-cache";
    CHECK(save_field_pair(outside, error), error.message.c_str());
    bool parent_linked = false;
#ifdef _WIN32
    parent_linked = create_junction(linked_parent, outside);
#else
    std::error_code parent_link_error;
    std::filesystem::create_directory_symlink(
        outside, linked_parent, parent_link_error);
    parent_linked = !parent_link_error;
#endif
    if (parent_linked) {
        auto incomplete = original;
        incomplete.state = hydrology::HydrologyNetworkState::Incomplete;
        incomplete.runtime_field_digest = 0u;
        incomplete.presentation_field_digest = 0u;
        incomplete.field_products.clear();
        incomplete.sections.clear();
        incomplete.handoffs.clear();
        incomplete.topological_order.clear();
        CHECK(!hydrology::save_network_artifact_atomic(
                  linked_parent / "river.mhydnet", incomplete, error) &&
                  !std::filesystem::exists(outside / "river.mhydnet"),
              "manifest publication rejects a reparse parent without outside mutation");
        std::error_code remove_error;
        std::filesystem::remove(linked_parent, remove_error);
    } else {
        std::printf("SKIP: platform could not create manifest parent reparse fixture\n");
    }

    const auto outside_leaf = outside / "manifest-external.mhydnet";
    const std::vector<std::uint8_t> outside_bytes = {1u, 2u, 3u, 4u};
    CHECK(write_bytes(outside_leaf, outside_bytes),
          "manifest leaf reparse target fixture was written");
    std::error_code leaf_error;
    std::filesystem::remove(path, leaf_error);
    leaf_error.clear();
    std::filesystem::create_symlink(outside_leaf, path, leaf_error);
    if (!leaf_error) {
        CHECK(!hydrology::save_network_artifact_atomic(path, original, error) &&
                  read_bytes(outside_leaf) == outside_bytes &&
                  std::filesystem::is_symlink(
                      std::filesystem::symlink_status(path)),
              "manifest publication rejects a reparse leaf without replacing its target");
        std::filesystem::remove(path, leaf_error);
    } else {
        std::printf("SKIP: platform could not create manifest leaf reparse fixture\n");
    }

    CHECK(hydrology::save_network_artifact_atomic(path, original, error),
          error.message.c_str());
    DirectoryNamespaceSwap swap{root, base / "cache-held", outside};
    hydrology::set_hydrology_namespace_validation_test_hook(
        attempt_directory_namespace_swap, &swap);
    const bool namespace_saved = hydrology::save_network_artifact_atomic(
        path, replacement, error);
    hydrology::set_hydrology_namespace_validation_test_hook(nullptr, nullptr);
#ifdef _WIN32
    CHECK(!namespace_saved && swap.invoked && swap.rename_succeeded,
          "Windows manifest publication remains handle-confined and fails closed after a parent swap");
#else
    CHECK(!namespace_saved && swap.invoked && swap.rename_succeeded,
          "manifest publication fails closed after a POSIX parent namespace swap");
#endif
    restore_directory_namespace(swap);

    const auto commit_path = root / "commit-boundary.mhydnet";
    const auto replacement_path = root / "manifest-replacement.tmp";
    const std::vector<std::uint8_t> replacement_bytes = {5u, 4u, 3u, 2u};
    CHECK(write_bytes(replacement_path, replacement_bytes),
          "manifest replacement fixture was written");
    ManifestCommitInterference interference{replacement_path};
    hydrology::set_hydrology_manifest_publication_test_hook(
        attempt_manifest_commit_interference, &interference);
    const bool commit_saved = hydrology::save_network_artifact_atomic(
        commit_path, original, error);
    hydrology::set_hydrology_manifest_publication_test_hook(nullptr, nullptr);
#ifdef _WIN32
    CHECK(commit_saved && interference.before_invoked &&
              !interference.write_succeeded &&
              !interference.delete_succeeded &&
              !interference.replace_succeeded &&
              read_bytes(commit_path) == original_bytes &&
              read_bytes(replacement_path) == replacement_bytes,
          "validated Windows manifest temporary denies write/delete/replace until exact bytes commit");
#else
    CHECK(commit_saved && interference.before_invoked &&
              read_bytes(commit_path) == original_bytes,
          "validated POSIX manifest descriptor publishes its exact bytes");
#endif

    const auto after_path = root / "after-commit.mhydnet";
    ManifestAfterCommitSwap after{{root, base / "cache-after-held", outside}};
    hydrology::set_hydrology_manifest_publication_test_hook(
        swap_manifest_parent_after_commit, &after);
    const bool after_saved = hydrology::save_network_artifact_atomic(
        after_path, original, error);
    hydrology::set_hydrology_manifest_publication_test_hook(nullptr, nullptr);
    CHECK(after_saved && after.invoked && after.swap.rename_succeeded &&
              read_bytes(after.swap.held / after_path.filename()) ==
                  original_bytes,
          "manifest create-new commit is final before observational postcommit work");
    restore_directory_namespace(after.swap);
    std::filesystem::remove_all(base);
}

} // namespace

int main() {
    test_manifest_round_trip_is_canonical_and_transactional();
    test_animation_manifest_extension_and_legacy_bytes();
    test_manifest_content_address_tracks_product_generation();
    test_ready_animation_package_rejects_missing_or_corrupt_payload();
    test_typed_field_wire_format_and_ready_package_closure();
    test_ready_package_rejects_reparse_escape_when_supported();
    test_allocation_failure_closes_native_resources_transactionally();
    test_file_identity_requires_one_stable_native_object();
    test_posix_publication_source_contract_uses_one_unprivileged_helper();
    test_confined_field_open_uses_the_held_directory_identity();
    test_manifest_publication_is_confined_and_transactional();
    return check_summary();
}
