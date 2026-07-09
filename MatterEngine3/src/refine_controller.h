#pragma once
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

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace matter_refine {

// ---------------------------------------------------------------------------
// Simple C++17-compatible span (std::span is C++20).
// ---------------------------------------------------------------------------
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
    uint64_t    resolved_hash = 0;
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
struct TileRecord {
    uint64_t coarse_hash = 0;
    uint64_t full_hash   = 0;
    float    pos[3] = {0, 0, 0};  // tile world center (instance translation + TILE_SIZE/2)
    enum class State { Coarse, Queued, Full } state = State::Coarse;
    uint32_t manifest_idx = 0;    // index of the coarse instance's manifest entry
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
    void build(span<const GraphNode> nodes, span<const InstanceRef> instances);

    size_t tile_count() const { return tiles_.size(); }

    // Number of tiles currently in State::Full.
    size_t full_count() const;

    // Highest-priority tile not yet Full/Queued, nearest to focus.
    // Returns false if none pending; sets *out to the record.
    bool next(const float focus[3], TileRecord** out);

    // Update a tile's state.  tile_idx < tile_count().
    void mark(uint32_t tile_idx, TileRecord::State s);

    // Full tiles farther than radius from focus, sorted farthest-first.
    // Coarse and Queued tiles are never included.
    std::vector<uint32_t> evict_beyond(const float focus[3], float radius) const;

private:
    std::vector<TileRecord> tiles_;

    // Return distance² from tile pos to focus.
    static float dist2(const float a[3], const float b[3]);
};

} // namespace matter_refine
