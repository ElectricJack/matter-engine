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
    // One LayerResult per script layer (in declaration order).
    // NOTE: the dropChild settle pass is NOT represented here — drops feed only
    // the aggregate converged_all flag, not an individual LayerResult entry.
    std::vector<LayerResult> layers;
    uint64_t pose_hash = 0;            // SettleWorld determinism hash
    bool from_cache = false;           // true when loaded from settle cache (no physics ran)
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

// Settle-result cache: serialize/restore a SettledTorus to/from disk.
// Cache file: <cache_root>/tileset/<key>.settle
// key = FNV-1a over (script_source_hash, sorted child resolved_hashes,
//                    kEngineBakeVersion, kBox3dVersion).
// Plain little-endian binary with a version header; reject on version/key mismatch.
bool settle_cache_load(const std::string& cache_root, uint64_t key, SettledTorus& out);
bool settle_cache_save(const std::string& cache_root, uint64_t key, const SettledTorus& s);

// Compute the settle cache key from its inputs.
// sorted_child_hashes must be sorted in ascending order by caller.
uint64_t settle_cache_key(uint64_t script_source_hash,
                          const std::vector<uint64_t>& sorted_child_hashes);

} // namespace tileset
