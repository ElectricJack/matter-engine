#pragma once

// Engine-side live terrain: turns world demand (a source program and a moving
// anchor) into region definitions for the ONE terrain coordinator, and hands
// the coordinator's products to three independent consumers:
//
//   cache     - the coordinator's own Cache (a repeated region reuses its product)
//   collision - collision() converts collision leases into the physics candidate
//               PhysicsContext::replace_terrain_collision installs
//   graphics  - the coordinator uploads through the supplied GraphicsService;
//               scene_delta() places each resident region in that service's scene
//
// No meshing, seam or LOD algorithm lives here: tiles are fixed-size, detail is
// a Chebyshev ring around the anchor, and each tile's boundary envelopes name
// its neighbours' current spacing so the generator applies its own seam policy.
// Staleness, cancellation and late completion are the coordinator's rules.
// Everything runs on the coordinator's application lane except the generation
// callback inside pump().

#include <matter/terrain_coordinator/coordinator.h>

#include "terrain_collision/terrain_collision_artifact.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace matter::terrain_coordinator {

struct LiveSettings {
    float tile_size_metres = 16;
    int32_t ground_key = 0;  // One vertical layer of tiles.
    // Chebyshev tile radius per detail ring; ring i samples at 2 * 2^i metres.
    // Tiles beyond the last radius are evicted.
    std::vector<uint32_t> ring_radii{1, 2};
    data::Capability boundary_policy{"matter.terrain.boundary.contour-2m", 1};
    Demand demand{false, true, true};
    std::string region_prefix = "terrain.live";
};

struct LiveRegion {
    data::SemanticId id;
    std::array<int32_t, 3> key{};
    float spacing_metres = 0;
};

class LiveTerrain {
public:
    LiveTerrain(Options, LiveSettings, Cache* = nullptr,
                services::GraphicsService* = nullptr);

    // The source program and per-request policy every tile shares. Region,
    // key, origin, domain, cells, detail and boundaries are filled per tile.
    // Changing it is an edit: every live tile is redefined, which cancels and
    // invalidates outstanding generation for the previous source.
    data::ValidationResult set_source(services::TerrainRequest prototype);
    // Re-plans the rings around a world position (metres). Only tiles whose
    // plan changed are redefined; tiles leaving the rings are evicted.
    data::ValidationResult set_anchor(const std::array<double, 3>& world_metres);
    // Releases every GPU residency. Definitions, cache and collision leases stay.
    data::ValidationResult drop_graphics();
    data::ValidationResult demand_graphics();
    // Runs queued generation inline, then advances graphics uploads once.
    data::ValidationResult pump(services::TerrainService&);

    // Collision for every live tile with a collision lease; false until at
    // least one exists. Keys change exactly when an installed tile changes.
    bool collision(terrain_collision::TerrainCollisionCandidate&,
                   std::string& error) const;
    // Scene changes since the last call: one instance per GPU-resident tile,
    // placed at its region origin.
    void scene_delta(std::vector<services::SceneInstance>& upsert,
                     std::vector<data::SemanticId>& remove);

    Coordinator& coordinator() { return coordinator_; }
    const Coordinator& coordinator() const { return coordinator_; }
    std::vector<LiveRegion> regions() const;
    const LiveRegion* region(const std::array<int32_t, 3>& key) const;

private:
    services::TerrainRequest request(const LiveRegion&) const;
    data::ValidationResult apply(std::map<std::array<int32_t, 3>, float> plan, bool edited);

    Options options_;
    LiveSettings settings_;
    Coordinator coordinator_;
    bool graphics_ = false;
    std::optional<services::TerrainRequest> source_;
    std::optional<std::array<double, 3>> anchor_;
    std::map<std::array<int32_t, 3>, LiveRegion> live_;
    std::map<std::string, std::pair<uint64_t, uint64_t>> placed_;  // id -> handle slot/generation
};

}  // namespace matter::terrain_coordinator
