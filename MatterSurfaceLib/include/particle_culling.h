#pragma once

#include "lattice.h"
#include "occupancy.h"
#include <vector>
#include <cstdint>

// A particle ready to hand to Cluster::add_particle (jitter already applied).
struct EmittedParticle {
    Vector3 position;   // local-space
    float radius;
    uint32_t materialId;
    Vector4 tint;       // RGBA; w = blend strength
};

struct CullParams {
    int margin;          // sub-shell layers to keep; clamped to >= 1
    float base_radius;   // nominal particle radius
    float jitter_amount; // per-axis position jitter magnitude (0 = none)
    float tint_alpha;    // tint blend strength written to EmittedParticle.tint.w
    uint32_t seed;       // determinism seed for jitter/tint
};

// Deterministic value-noise primitives (moved from main.cpp). [0,1] output.
float lattice_vhash(int x, int y, int z);
float lattice_vnoise(float x, float y, float z);

// A slot is buried iff every slot in the Chebyshev box of half-width `margin`
// around it is occupied. This currently hardcodes the grid's box neighborhood
// and does not consult lattice.neighbor_offsets(); future non-grid lattices
// would instead expand neighbor_offsets via BFS to `margin` steps.
bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin);

// Keep occupied slots that are NOT buried; emit a particle for each kept slot.
std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p);

// Baseline: emit a particle for every occupied slot (no culling). Used by the
// A/B acceptance comparison.
std::vector<EmittedParticle> emit_all(const Lattice& lattice,
                                      const Occupancy& occ,
                                      const CullParams& p);
