#pragma once

#include "lattice.h"
#include "occupancy.h"
#include "particle.h"
#include <vector>
#include <cstdint>

// A particle ready to hand to Cluster::add_particle (jitter already applied).
struct EmittedParticle {
    Vector3 position;   // local-space
    float radius;
    uint32_t materialId;
    Vector4 tint;       // RGBA; w = blend strength
    float detail_size;  // nominal lattice spacing at this particle's tier (S / 2^tier)
};

struct CullParams {
    int margin;          // sub-shell layers to keep; clamped to >= 1
    float base_radius;   // nominal particle radius
    float radius_variation = 0.0f; // particle radius scales by 1 +/- this (0 = uniform)
    float radius_cluster_freq = 0.0f; // value-noise frequency driving radius
                                      // clusters; low = big clumps of one scale
    float lump_amt = 0.0f;  // low-freq additive radius modulation (0 = off)
    float lump_freq = 0.0f; // lumpiness noise frequency (low = coarse bulges)
    float jitter_amount; // per-axis position jitter magnitude (0 = none)
    float tint_alpha;    // tint blend strength written to EmittedParticle.tint.w
    float vein_freq = 0.0f; // marble vein band frequency (0 = legacy random tint)
    float vein_warp = 0.0f; // how much turbulence meanders the veins
    int   max_tier = 0;   // 0 = one particle per slot (pre-feature behavior)
    float spacing  = 0.0f;// lattice tier-0 spacing S (GridLattice::spacing())
    uint32_t seed;       // determinism seed for jitter/tint
    float cell_size;     // meshing cell size used to bucket slots into cells
    Vector3 cell_origin_offset; // added to slot_position before bucketing so the
                                // cull grid matches the Cluster's recentered grid
    uint64_t coarse_material_mask = 0; // bit m set => material m emits one particle
                                       // per slot (forced tier 0), regardless of
                                       // max_tier. For discrete-geometry algorithms
                                       // (oriented cubes) where 8 sub-cubes/slot just
                                       // bloat the BVH without adding detail.
};

// Per-call statistics about the cell classification (optional output).
struct CullStats {
    size_t cells_total = 0;    // occupied cells
    size_t cells_meshed = 0;   // Surface cells (not interior) -> meshed
    size_t cells_skipped = 0;  // interior cells (Skin + Core) -> skip-meshed
    size_t cells_core = 0;     // core cells -> particles dropped
};

// Subtractive carve-particle generation. Seeded from surface particles; where a
// blended blob/ridge noise field exceeds (1 - amt), emit a negative particle
// whose radius scales with the overshoot (capped at r_max for watertightness).
struct CarveParams {
    float amt = 0.0f;          // 0 = off; threshold = 1 - amt
    float freq = 0.0f;         // carve noise frequency (feature spacing)
    float base_radius = 0.0f;  // base divot radius
    float ridge = 0.0f;        // 0 = round divots, 1 = linear crevices
    float r_max = 0.0f;        // watertight cap on carve radius (<=0 = uncapped)
    uint32_t seed = 0;
};

std::vector<Particle> generate_carve_particles(const std::vector<Particle>& seeds,
                                               const CarveParams& cp);

// Deterministic value-noise primitives (moved from main.cpp). [0,1] output.
float lattice_vhash(int x, int y, int z);
float lattice_vnoise(float x, float y, float z);

// A slot is buried iff every slot in the Chebyshev box of half-width `margin`
// around it is occupied. This currently hardcodes the grid's box neighborhood
// and does not consult lattice.neighbor_offsets(); future non-grid lattices
// would instead expand neighbor_offsets via BFS to `margin` steps.
bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin);

// Chebyshev depth below the surface: the largest k in [0, max_depth] such that
// every slot within Chebyshev radius k of c is occupied. depth 0 means an
// immediate box-neighbor is empty (outermost shell). Capped at max_depth so the
// scan stays O(max_depth^3). (slot_is_buried(c, m) == slot_depth(c, m) >= m.)
int slot_depth(const Occupancy& occ, SlotCoord c, int max_depth);

// Map a depth to a refinement tier: max_tier - min(depth, max_tier). The
// outermost shell (depth 0) gets the finest tier; depth >= max_tier gets tier 0.
int slot_tier(int depth, int max_tier);

// Cell-granular interior skip-meshing. Slots are bucketed into cells via
// floor((slot_position + cell_origin_offset) / cell_size). A cell is INTERIOR
// iff every slot in it is buried (slot_is_buried, margin>=1); a cell is CORE iff
// it is interior AND all 26 neighbor cells are present and interior.
//
// Emits a particle for every occupied slot EXCEPT those in core cells (core
// particles are never within sphere-reach of a meshed cell, so dropping them
// cannot perturb the outer surface). When `no_mesh_cells` is non-null it is
// filled with every interior cell's integer coordinate (the cells the cluster
// should create-but-not-mesh). When `stats` is non-null it gets the per-call
// cell counts. Interior cells keep their particles unless they are core, so
// every meshed (non-interior) cell is backed by particle-bearing neighbors and
// no inner surface forms.
std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p,
                                           CullStats* stats = nullptr,
                                           std::vector<SlotCoord>* no_mesh_cells = nullptr);

// Baseline: emit a particle for every occupied slot (no culling). Used by the
// A/B acceptance comparison.
std::vector<EmittedParticle> emit_all(const Lattice& lattice,
                                      const Occupancy& occ,
                                      const CullParams& p);
