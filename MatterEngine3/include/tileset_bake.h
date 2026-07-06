#pragma once
// tileset_bake.h — settle orchestrator: TilesetSpec → SettledTorus
//
// Instance ordering (test-guarded):
//   1. All shared drop instances (layer = -1), in DropChildRec order × 16 tiles.
//   2. Per script layer, in order:
//      a. Physics instances (strips + physics interiors) in settle spawn order.
//      b. Non-physics instances in placement order (strip[o][c] then interior[t]).
//
// strip occurrences are kept as ALL occurrence instances (Phase 3 renders each one).

#include "tileset_spec.h"
#include "tileset_settle.h"  // LayerResult, Pose
#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

struct SettledInstance {
    uint64_t child_hash = 0;
    float    scale = 1.0f;
    Pose     pose;            // torus-space (world XZ in [0, kTorusN*size))
    int      layer = -1;      // provenance: -1 = shared dropChild, else layer index
};

struct SettleReport {
    bool converged_all = true;
    std::vector<LayerResult> layers;   // per script layer
    uint64_t pose_hash = 0;            // SettleWorld determinism hash
};

struct SettledTorus {
    TileConfig cfg;
    BaseField  base;
    std::vector<SettledInstance> instances;   // spawn order (see ordering above)
    std::vector<VariantRange> variant_ranges; // pass-through for Phase 3
    SettleReport report;
};

struct BakeInputs {
    std::string parts_cache_dir;   // directory that CONTAINS parts/
};

// Assemble + settle the whole 4x4 torus.
// Fail-closed: returns false + err on any collider/load failure.
// Non-convergence is a WARNING in report.converged_all, not a hard error.
bool settle_tileset(const TilesetSpec& spec, const BakeInputs& in,
                    SettledTorus& out, std::string& err);

} // namespace tileset
