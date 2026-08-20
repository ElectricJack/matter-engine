// refine_controller.cpp — Phase C Task 4.
// Pure CPU data model for camera-driven tile refinement.
// See refine_controller.h for design notes.

// The whole class is a plain in-memory table: no I/O, no engine or GPU
// dependency, and no thread affinity of its own — `matter_engine.cpp` owns the
// single instance and drives it. It never bakes or evicts anything itself; it
// only decides WHICH tile is next, and the caller performs the work and reports
// back through mark().
//
// Terrain nodes are recognised by `module == "Terrain"` and paired by their
// `tx`/`tz` params, with `res == "coarse"` selecting the coarse slot and ANY
// other value the full slot. Params are read by scanning the canonical JSON
// string (see the helpers below), never by parsing it.
#include "refine_controller.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace matter_refine {

// ---------------------------------------------------------------------------
// Canonical-JSON string scanning helpers.
//
// Relies on params_to_json canonical form (part_graph::params_to_json):
//   • Keys are sorted alphabetically (std::map iteration order).
//   • Numbers use %.17g — integer values print without decimal point (e.g. 5, not 5.0).
//   • String values are quoted (e.g. "coarse").
//   • No whitespace.
// We exploit this by searching for the literal key substrings.
// ---------------------------------------------------------------------------

// Extract the string value for `key` from a canonical params_json.
// Returns empty string if not found.
// Example: json={"res":"coarse","tx":0,...}, key="res" -> "coarse"
static std::string extract_str(const std::string& json, const char* key) {
    // Search for "\"key\":\"" then extract up to the next '"'.
    std::string needle = std::string("\"") + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

// Extract an integer value for `key` from canonical params_json, with found flag.
// Returns (value, true) if found, (0, false) if not found.
static std::pair<int, bool> extract_int_or_missing(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {0, false};
    pos += needle.size();
    return {static_cast<int>(std::strtol(json.c_str() + pos, nullptr, 10)), true};
}

// ---------------------------------------------------------------------------
// build()
// ---------------------------------------------------------------------------

// Rebuild the tile table from scratch. INVALIDATES everything handed out
// before: `tiles_` is cleared, so any TileRecord* from a previous next() call
// dangles and any tile index from evict_beyond() may now mean a different tile.
// It also resets every tile to state Coarse, so refinement progress is not
// carried across a rebuild.
//
// A Terrain node missing `tx`, `tz` or `res` is skipped silently — the snapshot
// is authored data and one malformed node should not take out the grid.
//
// World position and manifest index come from the COARSE instance only. A tile
// whose coarse hash has no matching instance keeps pos (0,0,0), which makes its
// distance to the camera meaningless rather than infinite — it will look very
// close.
void RefineController::build(span<const GraphNode> nodes,
                              span<const InstanceRef> instances) {
    tiles_.clear();

    // Index instances by hash for O(1) lookup.
    std::unordered_map<uint64_t, const InstanceRef*> inst_by_hash;
    for (const auto& ir : instances) {
        inst_by_hash[ir.hash] = &ir;
    }

    // Collect Terrain nodes, separating coarse from full by (tx,tz) key.
    struct TileKey { int tx, tz; };
    struct TileAccum {
        uint64_t coarse_hash = 0;
        uint64_t full_hash   = 0;
    };

    // Use a map keyed by ((uint64_t)tx << 32) | tz so the two res-variants of
    // one tile land in the same slot, and so tiles come out in a deterministic
    // (tx, then tz) order — std::map iterates by key, not by insertion.
    // tx/tz range is 0..50 for the 51×51 Meadow Valley; no collisions.
    std::map<uint64_t, TileAccum> by_tile;

    for (const auto& node : nodes) {
        if (node.module != "Terrain") continue;

        // Extract required keys with presence checking; skip malformed nodes.
        auto [tx, tx_found] = extract_int_or_missing(node.params_json, "tx");
        auto [tz, tz_found] = extract_int_or_missing(node.params_json, "tz");
        std::string res = extract_str(node.params_json, "res");

        if (!tx_found || !tz_found || res.empty()) {
            // Malformed Terrain node (missing tx, tz, or res) — skip it.
            continue;
        }

        uint64_t key = ((uint64_t)(uint32_t)tx << 32) | (uint64_t)(uint32_t)tz;
        TileAccum& acc = by_tile[key];

        if (res == "coarse") {
            acc.coarse_hash = node.resolved_hash;
        } else {
            // "full" or any other res variant
            acc.full_hash = node.resolved_hash;
        }
    }

    // Build TileRecord for each paired tile.
    tiles_.reserve(by_tile.size());
    for (const auto& kv : by_tile) {
        const TileAccum& acc = kv.second;

        // Recover (tx, tz) from the map key (upper/lower 32 bits).
        int rec_tx = (int)(uint32_t)(kv.first >> 32);
        int rec_tz = (int)(uint32_t)(kv.first & 0xFFFFFFFFu);

        TileRecord rec;
        rec.coarse_hash = acc.coarse_hash;
        rec.full_hash   = acc.full_hash;
        rec.state       = TileRecord::State::Coarse;
        rec.pos[0] = rec.pos[1] = rec.pos[2] = 0.0f;
        rec.manifest_idx = 0;
        rec.tile_tx = rec_tx;
        rec.tile_tz = rec_tz;

        // Match coarse instance to get world position + manifest_idx.
        if (acc.coarse_hash != 0) {
            auto it = inst_by_hash.find(acc.coarse_hash);
            if (it != inst_by_hash.end()) {
                const InstanceRef& ir = *it->second;
                // Tile center = instance origin + TILE_SIZE/2 on X and Z axes.
                rec.pos[0] = ir.translation[0] + TILE_SIZE * 0.5f;
                rec.pos[1] = ir.translation[1];
                rec.pos[2] = ir.translation[2] + TILE_SIZE * 0.5f;
                rec.manifest_idx = ir.manifest_idx;
            }
        }

        tiles_.push_back(rec);
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

float RefineController::dist2(const float a[3], const float b[3]) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return dx*dx + dy*dy + dz*dz;
}

// O(tiles) — counts on every call, nothing is cached. Called per frame today,
// which is fine at grid sizes in the thousands.
size_t RefineController::full_count() const {
    size_t n = 0;
    for (const auto& t : tiles_) {
        if (t.state == TileRecord::State::Full) ++n;
    }
    return n;
}

// Pick the Coarse tile nearest `focus` (world-space XYZ). Returns false and
// leaves *out null when no tile is still Coarse — the normal "fully refined"
// outcome, not an error.
//
// PURE QUERY: it does not change the tile's state, so a caller that does not
// mark() the returned tile Queued/Full is handed the same tile again on the
// next call. The returned pointer points into `tiles_` and is invalidated by
// the next build().
bool RefineController::next(const float focus[3], TileRecord** out) {
    *out = nullptr;
    float best_d2 = -1.0f;
    TileRecord* best = nullptr;

    for (auto& t : tiles_) {
        if (t.state != TileRecord::State::Coarse) continue;
        float d2 = dist2(focus, t.pos);
        if (best == nullptr || d2 < best_d2) {
            best_d2 = d2;
            best    = &t;
        }
    }

    if (best == nullptr) return false;
    *out = best;
    return true;
}

// `tile_idx` indexes `tiles_` in the order build() produced (the same index
// space evict_beyond returns). Out-of-range indices are ignored silently, so a
// stale index from before a rebuild fails quietly rather than corrupting a
// neighbour.
void RefineController::mark(uint32_t tile_idx, TileRecord::State s) {
    if (tile_idx < tiles_.size()) {
        tiles_[tile_idx].state = s;
    }
}

// Every FULL tile whose center is farther than `radius` from `focus`, as tile
// indices, sorted farthest-first so a caller with a budget can evict the worst
// offenders and stop. Const and side-effect-free: the tiles stay in state Full
// until the caller marks them.
std::vector<uint32_t> RefineController::evict_beyond(const float focus[3],
                                                       float radius) const {
    float r2 = radius * radius;

    std::vector<std::pair<float, uint32_t>> candidates;
    for (uint32_t i = 0; i < (uint32_t)tiles_.size(); ++i) {
        if (tiles_[i].state != TileRecord::State::Full) continue;
        float d2 = dist2(focus, tiles_[i].pos);
        if (d2 > r2) {
            candidates.push_back({d2, i});
        }
    }

    // Sort farthest-first (descending by d2).
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<uint32_t> result;
    result.reserve(candidates.size());
    for (const auto& c : candidates) {
        result.push_back(c.second);
    }
    return result;
}

} // namespace matter_refine
