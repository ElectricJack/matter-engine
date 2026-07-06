#pragma once
#include <cstdint>
#include <vector>

namespace tileset {

struct Point2 { float x, z; };   // domain-local

enum class PlacementKind { Uniform = 0, Poisson = 1, Cluster = 2 };

// Rectangular domain [x0,x1) x [z0,z1) with corner-clear disks.
struct PlacementDomain {
    float x0, x1, z0, z1;
    // Disk centers (domain-local) that placements must clear by `clear_radius`.
    std::vector<Point2> clear_disks;
    float clear_radius = 0.0f;
};

// Deterministic scatter: expected count = density * usable area (rect area; the
// clear disks are handled by rejection). Same seed + domain + kind => same output.
std::vector<Point2> scatter(PlacementKind kind, const PlacementDomain& dom,
                            float density, uint64_t seed);

// Seed folding used by every placement call site (documented, test-guarded):
// fold(master, layer_index, domain_id) with SplitMix64 avalanche per fold.
uint64_t placement_seed(uint64_t master, uint32_t layer_index, uint32_t domain_id);

} // namespace tileset
