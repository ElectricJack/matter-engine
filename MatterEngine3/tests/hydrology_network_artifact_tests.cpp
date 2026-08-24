#include "check.h"

#include "hydrology/hydrology_network_artifact.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

hydrology::HydrologyNetworkArtifact fixture_manifest(bool reversed = false) {
    hydrology::HydrologyNetworkArtifact manifest{};
    manifest.state = hydrology::HydrologyNetworkState::Ready;
    manifest.network_key = 101u;
    manifest.terrain_revision = 202u;
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

void test_manifest_round_trip_is_canonical_and_fail_closed() {
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
              reopened.payload_digest != 0u,
          "ready manifest identity and canonical references round-trip");

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

    auto corrupt = bytes;
    corrupt.back() ^= 0x80u;
    CHECK(!hydrology::deserialize_network_artifact(corrupt, reopened, error),
          "manifest payload corruption is rejected");
    corrupt = bytes;
    corrupt.resize(corrupt.size() - 1u);
    CHECK(!hydrology::deserialize_network_artifact(corrupt, reopened, error),
          "truncated manifests are rejected");
}

void test_atomic_ready_load_rejects_incomplete_state() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("matter-hydrology-network-" + std::to_string(stamp));
    const auto path = root / "river.mhydnet";
    gpu_meshing::Error error{};
    CHECK(hydrology::save_network_artifact_atomic(
              path, fixture_manifest(), error),
          error.message.c_str());
    hydrology::HydrologyNetworkArtifact loaded{};
    CHECK(hydrology::load_network_artifact_validated(
              path, 101u, 202u, loaded, error) &&
              loaded.state == hydrology::HydrologyNetworkState::Ready,
          error.message.c_str());

    auto incomplete = fixture_manifest();
    incomplete.state = hydrology::HydrologyNetworkState::Incomplete;
    incomplete.sections.clear();
    incomplete.handoffs.clear();
    incomplete.topological_order.clear();
    CHECK(hydrology::save_network_artifact_atomic(path, incomplete, error),
          error.message.c_str());
    CHECK(!hydrology::load_network_artifact_validated(
              path, 101u, 202u, loaded, error),
          "an incomplete manifest is persisted for diagnostics but is never ready");
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_manifest_round_trip_is_canonical_and_fail_closed();
    test_atomic_ready_load_rejects_incomplete_state();
    return check_summary();
}
