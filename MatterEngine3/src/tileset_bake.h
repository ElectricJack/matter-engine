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
#include "tileset_settle.h"  // LayerResult, Pose, BodySpawn, HeightField
#include <cstdint>
#include <map>
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

// ---------------------------------------------------------------------------
// Settle plan — everything settle_tileset feeds the SettleWorld, prebuilt.
// Shared by the batch bake and the interactive Settle Lab so both simulate
// exactly the same inputs (settle-tick-optimizer.md §II.2).
// ---------------------------------------------------------------------------

// Scaled-collider memoization key: (child_hash, collider_override, scale).
// Scale derives deterministically from the RNG so exact float comparison is safe.
struct ScaledColliderKey {
    uint64_t    child_hash;
    std::string override_str;
    float       scale;
    bool operator<(const ScaledColliderKey& o) const {
        if (child_hash != o.child_hash) return child_hash < o.child_hash;
        if (override_str != o.override_str) return override_str < o.override_str;
        return scale < o.scale;
    }
};

// Provenance record for each spawned physics body (parallel to its BodySpawn).
struct SpawnProv {
    uint64_t child_hash = 0;
    float    scale      = 1.0f;
    int      layer      = -1;   // -1 = drop
};

// Non-physics placement, analytically snapped to the base at plan time.
struct NonPhysInst {
    uint64_t child_hash = 0;
    float    scale      = 1.0f;
    Pose     pose;
    int      layer      = -1;
};

// LIFETIME: `colliders` OWNS the ColliderFit objects that every
// BodySpawn::collider pointer in `drop_spawns` / `layers[*].spawns` borrows.
// The SettlePlan must therefore outlive any SettleWorld run consuming those
// spawns. std::map node stability keeps the pointers valid across further
// insertions and across moves of the SettlePlan itself; copying a SettlePlan
// would NOT rebind the pointers, so treat it as move-only in practice.
struct SettlePlan {
    float       torus_size = 0.0f;               // kTorusN * cfg.size
    HeightField hf;                              // tiled torus heightfield
    // Collider storage: owns what BodySpawn::collider pointers reference.
    std::map<ScaledColliderKey, ColliderFit> colliders;
    // Sync-group occurrence frames, in add order: plan index == the group id
    // SettleWorld::add_sync_group returns when groups are added in this order,
    // which is what BodySpawn::sync_group references.
    std::vector<std::vector<Pose>> sync_group_frames;
    std::vector<BodySpawn> drop_spawns;          // shared drops, one batch
    std::vector<SpawnProv> drop_provs;           // parallel to drop_spawns
    struct LayerPlan {
        std::string module;                      // for UI labels
        bool physics = false;
        std::vector<BodySpawn>   spawns;         // physics only
        std::vector<SpawnProv>   provs;          // parallel to spawns
        std::vector<NonPhysInst> nonphys;        // pass-through placements
    };
    std::vector<LayerPlan> layers;               // one per script layer, in order
};

// Build the settle plan (heightfield, colliders, sync groups, spawn lists,
// provenance) without creating a physics world. Pure reorganization of what
// settle_tileset feeds SettleWorld: no RNG draws, no reordering — placements
// already happened during eval.
// Fail-closed: returns false + err on any collider/load failure.
bool build_settle_plan(const TilesetSpec& spec, const BakeInputs& in,
                       SettlePlan& out, std::string& err);

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
uint64_t settle_cache_key(uint64_t script_source_hash,
                          const std::vector<uint64_t>& sorted_child_hashes,
                          const std::string& canonical_root_params_json);

} // namespace tileset
