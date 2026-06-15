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
    float radius_variation = 0.0f; // per-particle radius scales by 1 +/- this (0 = uniform)
    float jitter_amount; // per-axis position jitter magnitude (0 = none)
    float tint_alpha;    // tint blend strength written to EmittedParticle.tint.w
    uint32_t seed;       // determinism seed for jitter/tint
    float cell_size;     // meshing cell size used to bucket slots into cells
    Vector3 cell_origin_offset; // added to slot_position before bucketing so the
                                // cull grid matches the Cluster's recentered grid
};

// Per-call statistics about the cell classification (optional output).
struct CullStats {
    size_t cells_total = 0;    // occupied cells
    size_t cells_meshed = 0;   // Surface cells (not interior) -> meshed
    size_t cells_skipped = 0;  // interior cells (Skin + Core) -> skip-meshed
    size_t cells_core = 0;     // core cells -> particles dropped
};

// Deterministic value-noise primitives (moved from main.cpp). [0,1] output.
float lattice_vhash(int x, int y, int z);
float lattice_vnoise(float x, float y, float z);

// A slot is buried iff every slot in the Chebyshev box of half-width `margin`
// around it is occupied. This currently hardcodes the grid's box neighborhood
// and does not consult lattice.neighbor_offsets(); future non-grid lattices
// would instead expand neighbor_offsets via BFS to `margin` steps.
bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin);

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
