#pragma once
// Engine-side adapter: hands part sources that declare
//   static authoring = "matter.authoring.portable.v1";
// from part_graph::HostBaker to the portable authoring pipeline
// (QuickJS host -> Coordinator -> geometry) and publishes the result as an
// ordinary .part. The kernel only sees part_graph::PortableBakeRoute.
#include "part_graph.h"
#include <cstdint>
#include <memory>
#include <string>
#if defined(MATTER_HAVE_SCRIPT_HOST)
namespace mm { struct Mat4; }
namespace matter::contracts { struct AssetDocument; struct Instance; }
namespace matter::services { struct AssetProduct; }
namespace matter::authoring_adapter {
std::shared_ptr<part_graph::PortableBakeRoute> make_legacy_part_route();

// The immutable product a portable part hash was built from, retained for the
// graphics adapter (component_adapters/graphics). Null for legacy parts and for
// hashes this process never resolved. A warm world resolve cache re-authors its
// portable nodes on restore (LocalProvider::restore_from_cache), so every
// portable hash a world admits has been resolved here.
std::shared_ptr<const matter::services::AssetProduct> retained_asset(uint64_t part_hash);

// Why a portable hash that resolved has no retained product (the coordinator's
// diagnostic); empty when it has one or was never resolved here.
std::string retained_refusal(uint64_t part_hash);

// Advances whenever a product is retained or refused, so a consumer whose
// instance list did not change can still tell a late product has arrived.
uint64_t retained_generation();

// Engine palette meaning of a portable material id. Builtin ids carry the
// frozen MaterialRegistry factors; dynamic ids name per-world registry slots
// whose factors the recipe cannot freeze (the .part keeps the numeric id for
// the legacy renderer); anything else has no engine palette id.
enum class LegacyMaterialKind { builtin, dynamic, unknown };
LegacyMaterialKind classify_legacy_material(const std::string& material_id, int* id_out = nullptr);

// Why the graphics adapter must not draw `asset`: the first submesh material
// that is not builtin. Empty when every material carries immutable factors.
std::string legacy_material_refusal(const matter::contracts::AssetDocument& asset);

// Part-space transform of an asset instance: its parent chain composed
// parent * local, as SceneBackend::expand composes assembly children. False
// (and logged) on a broken or cyclic chain.
bool compose_instance_transform(const matter::contracts::AssetDocument& asset,
                                const matter::contracts::Instance& instance, mm::Mat4& world);
} // namespace matter::authoring_adapter
#endif
