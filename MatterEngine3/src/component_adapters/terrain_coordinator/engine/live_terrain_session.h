#pragma once

// The editor's owner of live terrain: one LiveTerrain, the coordinator cache
// it reuses products from, and the portable CPU terrain service for the
// world's current field program. WorldSession drives it once per frame on the
// app thread (matter_engine.cpp, "build.live_terrain"); this file only turns a
// field program + revision into the shared request policy and routes work.

#include "component_adapters/terrain_coordinator/engine/live_terrain.h"

#include <matter/terrain/field.h>

#include <memory>

namespace matter::terrain_coordinator {

class LiveTerrainSession {
public:
    explicit LiveTerrainSession(services::GraphicsService* graphics, LiveSettings = {});

    // A world install or edit: the field program every tile samples. An
    // unchanged program is a no-op; a changed one redefines every live tile.
    data::ValidationResult set_source(const std::string& program, const std::string& revision);
    // One frame of demand: re-plan around `anchor`, drop or re-demand GPU
    // residency, then run queued generation and advance uploads.
    data::ValidationResult update(const std::array<double, 3>& anchor, bool graphics);
    // The current collision candidate, only when its key differs from `installed`.
    bool collision_changed(uint64_t installed, terrain_collision::TerrainCollisionCandidate&,
                           std::string& error) const;

    LiveTerrain& live() { return *live_; }
    services::TerrainService& service() { return *service_; }
    MemoryCache& cache() { return cache_; }
    bool has_source() const { return service_ != nullptr; }
    // Tiles the CPU generator actually sampled; a cache hit does not count.
    uint64_t generated() const { return generated_; }

private:
    // Entries reserve their DECLARED worst case (output + resource + document
    // budgets: 192 MiB per tile), not their real size, so the entry count is
    // the effective bound: 256 tiles is ten 5x5 neighbourhoods.
    MemoryCache cache_{256, 256 * (192ull << 20)};
    std::unique_ptr<services::TerrainService> service_;  // Generates only its own field.
    float tile_;
    std::unique_ptr<LiveTerrain> live_;
    std::string content_;
    bool graphics_ = true;
    uint64_t generated_ = 0;
};

}  // namespace matter::terrain_coordinator
