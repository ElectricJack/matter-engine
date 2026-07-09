// refine_controller.cpp — Phase C Task 4.
// Pure CPU data model for camera-driven tile refinement.
// See refine_controller.h for design notes.

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

    // Use a map keyed by (tx*65536+tz) so pairs stay insertion-ordered.
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

        TileRecord rec;
        rec.coarse_hash = acc.coarse_hash;
        rec.full_hash   = acc.full_hash;
        rec.state       = TileRecord::State::Coarse;
        rec.pos[0] = rec.pos[1] = rec.pos[2] = 0.0f;
        rec.manifest_idx = 0;

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

size_t RefineController::full_count() const {
    size_t n = 0;
    for (const auto& t : tiles_) {
        if (t.state == TileRecord::State::Full) ++n;
    }
    return n;
}

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

void RefineController::mark(uint32_t tile_idx, TileRecord::State s) {
    if (tile_idx < tiles_.size()) {
        tiles_[tile_idx].state = s;
    }
}

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
