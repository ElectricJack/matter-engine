#pragma once
#include <matter/asset/collision.h>
#include <array>

namespace matter::terrain_collision { struct TerrainCollisionCandidate; }
namespace matter::component_adapters::collision {
namespace portable = matter::asset::collision;
// Neutral terrain product: identities retain all 64 bits; vertices remain tile
// local. Empty tiles survive and do not depend on graphics residency.
struct Tile {
    std::array<int64_t,3> coordinate{};
    uint64_t key=0, digest=0;
    matter::contracts::SemanticId body;
};
struct TerrainProduct {
    uint32_t version=1;
    uint64_t geometry_key=0, installation_key=0;
    float friction=0.7f, restitution=0;
    portable::Product collision;
    std::vector<Tile> tiles;
};
matter::contracts::ValidationResult from_candidate(const matter::terrain_collision::TerrainCollisionCandidate&, TerrainProduct&);
matter::contracts::ValidationResult to_candidate(const TerrainProduct&, matter::terrain_collision::TerrainCollisionCandidate&);
} // namespace matter::component_adapters::collision
