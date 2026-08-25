#include "check.h"

#include "hydrology/hydrology_field_artifact.h"
#include "hydrology/hydrology_network_artifact.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
         "fields/runtime.mhydfield", runtime.payload_digest},
        {hydrology::HydrologyFieldProductKind::Presentation,
         "fields/presentation.mhydfield", presentation.payload_digest},
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
    const auto manifest = fixture_manifest();
    return hydrology::save_hydrology_field_product_atomic(
               root / manifest.field_products[0].relative_path,
               fixture_runtime_product(), error) &&
           hydrology::save_hydrology_field_product_atomic(
               root / manifest.field_products[1].relative_path,
               fixture_presentation_product(), error);
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
    CHECK(save_field_pair(root, error), error.message.c_str());

    auto truncated_runtime = read_bytes(runtime_path);
    truncated_runtime.pop_back();
    CHECK(write_bytes(runtime_path, truncated_runtime),
          "truncated runtime fixture was written");
    rejected_load_preserves("a Ready manifest with a truncated field file is rejected");
    CHECK(save_field_pair(root, error), error.message.c_str());

    const auto valid_runtime_bytes = read_bytes(runtime_path);
    const auto valid_presentation_bytes = read_bytes(presentation_path);
    CHECK(write_bytes(runtime_path, valid_presentation_bytes) &&
              write_bytes(presentation_path, valid_runtime_bytes),
          "swapped typed field fixtures were written");
    rejected_load_preserves("swapped runtime/presentation field types are rejected");
    CHECK(save_field_pair(root, error), error.message.c_str());

    auto stale_runtime = fixture_runtime_product();
    stale_runtime.gameplay[0].height_m += 1.0f;
    stale_runtime.payload_digest = hydrology::hydrology_runtime_field_digest(
        stale_runtime.layout, stale_runtime.gameplay);
    CHECK(hydrology::save_hydrology_field_product_atomic(
              runtime_path, stale_runtime, error), error.message.c_str());
    rejected_load_preserves("a structurally valid stale-digest field is rejected");
    CHECK(save_field_pair(root, error), error.message.c_str());

    CHECK(!hydrology::load_network_artifact_validated(
              path, 999u, 202u, loaded, error),
          "a stale Ready network key is rejected");
    check_manifest_preserved(
        loaded, loaded_sentinel,
        "stale identity rejection preserves the caller's prior manifest");

    for (const auto& entry : std::filesystem::directory_iterator(root / "fields"))
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
    CHECK(hydrology::save_network_artifact_atomic(path, incomplete, error),
          error.message.c_str());
    CHECK(!hydrology::load_network_artifact_validated(
              path, 101u, 202u, loaded, error),
          "an incomplete manifest is persisted for diagnostics but is never ready");
    check_manifest_preserved(
        loaded, loaded_sentinel,
        "incomplete Ready loads preserve the caller's prior manifest");
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_manifest_round_trip_is_canonical_and_transactional();
    test_typed_field_wire_format_and_ready_package_closure();
    return check_summary();
}
