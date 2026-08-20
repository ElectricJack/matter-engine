#pragma once
// MatterEngine3/src/refine_controller.h
// RefineController — Phase C Task 4.
// Pure CPU data model that pairs coarse/full Terrain nodes by (tx,tz) and
// answers priority/eviction queries for camera-driven tile refinement (Task 6).
//
// The snapshot is populated at resolve time (BakePolicy::RootsOnly, Task 13):
// every Terrain node — including full-res variants not yet baked — appears with
// a resolved_hash and a canonical params_json.  RefineController scans those
// nodes, pairs them, and cross-references the world instance list to obtain
// tile world-space centers and manifest indices.
//
// Canonical params dependency:
//   params_json strings must be produced by part_graph::params_to_json (sorted
//   std::map keys, %.17g numbers, "str" strings, no whitespace).  Integer values
//   print without a decimal point (e.g. "tx":5 not "tx":5.0).  The string-scan
//   helpers below rely on this form — they are not general JSON parsers.

// Lifecycle and ownership:
//   matter_engine.cpp owns the one live instance (a std::unique_ptr) and
//   rebuilds it from scratch after each resolve/bake, constructing a fresh
//   controller and calling build() on it.  build() clears and repopulates the
//   tile array, so every TileRecord* previously returned by next() and every
//   tile index previously handed to mark()/tile_at() is invalidated by the
//   next build().
//
// Threading:
//   No synchronisation of any kind, and no GPU or OS resources — this is plain
//   std::vector state touched only from the thread that drives the refine step.
//
// Units and spaces:
//   pos[] and TILE_SIZE are world units on the same axes as the instance
//   transforms fed to build(); tile_tx/tile_tz are integer Terrain tile
//   indices, not positions.  The distances used by next() and evict_beyond()
//   are 3D, so a focus point above the ground plane eats into the effective
//   XZ radius (each method spells this out).
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace matter_refine {

// ---------------------------------------------------------------------------
// Simple C++17-compatible span (std::span is C++20).
// ---------------------------------------------------------------------------
// Non-owning view — the caller keeps the backing array alive.  build() copies
// what it needs into its own storage, so a span passed to it only has to stay
// valid for the duration of that call.
template<typename T>
struct span {
    const T* data = nullptr;
    size_t   size = 0;
    span() = default;
    span(const T* d, size_t n) : data(d), size(n) {}
    const T* begin() const { return data; }
    const T* end()   const { return data + size; }
};

// ---------------------------------------------------------------------------
// Input types consumed by build().
// ---------------------------------------------------------------------------

// One resolved node from the graph snapshot.
struct GraphNode {
    std::string module;       // e.g. "Terrain", "Scatter", "Grass"
    std::string params_json;  // canonical JSON produced by params_to_json
    uint64_t    resolved_hash = 0;  // part identity; matched against InstanceRef::hash
};

// One placed instance from the world manifest.
// translation[3] holds the world-space XYZ of the placed transform.
// (Engine matrices are ROW-major: translation at indices [3],[7],[11];
//  this struct abstracts that — callers extract those three floats.)
struct InstanceRef {
    uint64_t hash = 0;         // resolved_hash of the placed part
    float    translation[3];   // world-space XYZ origin (NOT the tile center)
    uint32_t manifest_idx = 0; // index of this instance's manifest entry
};

// ---------------------------------------------------------------------------
// Per-tile record maintained by RefineController.
// ---------------------------------------------------------------------------
// One refinement tile: the coarse / full-res pair of Terrain nodes that share a
// (tx,tz).  The two hash fields are independently optional — a tile can be
// known to the graph with only one of its variants present — and pos/
// manifest_idx are filled in only from the COARSE instance, so a tile whose
// coarse variant was never placed keeps its defaults.
struct TileRecord {
    uint64_t coarse_hash = 0;  // resolved_hash of the coarse variant; 0 = absent
    uint64_t full_hash   = 0;  // resolved_hash of the full-res variant; 0 = absent
    // Only meaningful when `placed` is true. Stays {0,0,0} otherwise, which is
    // why `placed` exists: {0,0,0} is a perfectly ordinary world position, so
    // the flag is the only thing that separates "tile at the origin" from "tile
    // whose coarse variant is nowhere in the manifest".
    float    pos[3] = {0, 0, 0};  // tile world center (instance translation + TILE_SIZE/2)
    // True once build() matched `coarse_hash` to a manifest instance and filled
    // in pos/manifest_idx from it. False means the graph named a tile the world
    // never placed: its pos would read as the origin (so next() would rank it
    // first) and its manifest_idx would read as 0 (so a refine would swap a
    // DIFFERENT instance's part hash). next() therefore skips unplaced tiles
    // entirely -- they are not refinable, and there is nothing to evict.
    bool     placed = false;
    // Coarse — only the coarse variant is placed.  The ONLY state next() will
    //          return, so a tile has to be moved out of it (normally to Queued)
    //          to stop being handed back again.
    // Queued — a refine step has been issued for this tile.  Skipped by both
    //          next() and evict_beyond(), i.e. in-flight work is never picked
    //          twice and never evicted.
    // Full   — the full-res variant is resident.  The only state evict_beyond()
    //          will report.
    enum class State { Coarse, Queued, Full } state = State::Coarse;
    uint32_t manifest_idx = 0;    // index of the coarse instance's manifest entry
    int      tile_tx = 0;         // Terrain tx param (for event identity / test assertions)
    int      tile_tz = 0;         // Terrain tz param (for event identity / test assertions)
};

// ---------------------------------------------------------------------------
// RefineController
// ---------------------------------------------------------------------------
class RefineController {
public:
    // TILE_SIZE in world units; coarse instances are translated by the engine at
    // multiples of this.  Tile center = translation + TILE_SIZE/2 per XZ axis.
    static constexpr float TILE_SIZE = 10.0f;

    // Pair Terrain nodes by (tx,tz) across res variants; match world instances
    // by hash to populate TileRecord positions and manifest indices.
    // Non-Terrain module names are ignored silently.
    // Terrain nodes whose params_json is missing tx or tz are skipped the same
    // way, so a malformed graph yields fewer tiles rather than an error.
    // Rebuilds from scratch — all previously stored tiles, indices and pointers
    // are dropped.  Linear in nodes + instances, and allocates.
    void build(span<const GraphNode> nodes, span<const InstanceRef> instances);

    size_t tile_count() const { return tiles_.size(); }

    // Read-only access to all tile records (indexed by tile_idx).
    // Used by execute_refine_step to obtain coarse_hash, full_hash, manifest_idx
    // by the same index that mark() and evict_beyond() use.
    const TileRecord& tile_at(uint32_t tile_idx) const { return tiles_[tile_idx]; }

    // Number of tiles currently in State::Full.
    // Counts by scanning every tile; no running counter is maintained.
    size_t full_count() const;

    // Highest-priority PLACED tile not yet Full/Queued, nearest to focus.
    // Tiles with placed == false are never returned (see TileRecord::placed).
    // Distance is 3D (includes y); for ground-plane tiles with pos[1]=0, caller must account
    // for camera height when comparing distances.
    // Returns false if none pending; sets *out to the record.
    // Scans every tile (Queued and Full are skipped) — there is no priority
    // queue behind this.  *out points into the controller's own tile array, so
    // it is invalidated by the next build(); convert it to an index with
    // tile_index_of() if it has to outlive the immediate use.
    bool next(const float focus[3], TileRecord** out);

    // Return the index of a TileRecord returned by next().
    // Precondition: tr must be a pointer into this controller's tile array.
    uint32_t tile_index_of(const TileRecord* tr) const {
        return (uint32_t)(tr - tiles_.data());
    }

    // Update a tile's state.  tile_idx < tile_count().
    // An out-of-range index is ignored silently — no assert, no return value,
    // so a stale index from before a build() simply does nothing.
    void mark(uint32_t tile_idx, TileRecord::State s);

    // Full tiles farther than radius from focus, sorted farthest-first.
    // Distance is 3D (includes y); for ground-plane tiles with pos[1]=0, caller must account
    // for camera height when choosing radius (effective XZ-radius shrinks to sqrt(radius²−H²)).
    // Coarse and Queued tiles are never included.
    // Builds and sorts a temporary vector: allocating, and O(n log n) in the
    // number of Full tiles.  The returned indices are only meaningful until the
    // next build().
    std::vector<uint32_t> evict_beyond(const float focus[3], float radius) const;

private:
    std::vector<TileRecord> tiles_;

    // Return distance² from tile pos to focus.
    static float dist2(const float a[3], const float b[3]);
};

} // namespace matter_refine
