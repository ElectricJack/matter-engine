#pragma once
// Engine-side adapter: hands part sources that declare
//   static authoring = "matter.authoring.portable.v1";
// from part_graph::HostBaker to the portable authoring pipeline
// (QuickJS host -> Coordinator -> geometry) and publishes the result as an
// ordinary .part. The kernel only sees part_graph::PortableBakeRoute.
#include "part_graph.h"
#include <cstdint>
#include <memory>
#if defined(MATTER_HAVE_SCRIPT_HOST)
namespace mm { struct Mat4; }
namespace matter::contracts { struct AssetDocument; struct Instance; }
namespace matter::services { struct AssetProduct; }
namespace matter::authoring_adapter {
std::shared_ptr<part_graph::PortableBakeRoute> make_legacy_part_route();

// The immutable product a portable part hash was built from, retained for the
// graphics adapter (component_adapters/graphics). Null for legacy parts and for
// hashes this process never resolved (a warm world resolve cache skips them).
std::shared_ptr<const matter::services::AssetProduct> retained_asset(uint64_t part_hash);

// Part-space transform of an asset instance: its parent chain composed
// parent * local, as SceneBackend::expand composes assembly children. False
// (and logged) on a broken or cyclic chain.
bool compose_instance_transform(const matter::contracts::AssetDocument& asset,
                                const matter::contracts::Instance& instance, mm::Mat4& world);
} // namespace matter::authoring_adapter
#endif
