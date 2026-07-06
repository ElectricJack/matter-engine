#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dsl_rng.h"

namespace tileset {

struct TileConfig {
    float    size = 2.0f;               // meters per tile edge
    int      texels_per_meter = 512;
    uint64_t seed = 0;
    float    edge_strip_width = 0.15f;  // m
    float    corner_clear_radius = 0.08f;
};

// base(fn, material): heightfield sampled during eval on a per-tile periodic grid.
// heights[z*n + x] = fn(x*cell, z*cell); the grid tiles toroidally (sample n wraps to 0).
struct BaseField {
    static constexpr int kSamplesPerTile = 64;
    int      n = 0;          // samples per edge (kSamplesPerTile when set)
    float    cell = 0.0f;    // size / n
    uint32_t material = 0;
    std::vector<float> heights;
    bool     set = false;
};

// One resolved instance placement. Coordinates are DOMAIN-LOCAL:
//  - strip placements: x = across-seam offset in [-w, +w], z = along-seam in [0, size), y = drop height (physics) / 0 (snapped later)
//  - interior placements: x,z = tile-local in [w, size-w), y likewise
struct Placement {
    uint64_t child_hash = 0;
    float pos[3]  = { 0, 0, 0 };
    float quat[4] = { 0, 0, 0, 1 };  // xyzw
    float scale   = 1.0f;
};

struct LayerSpec {
    std::string module;
    float density = 0.0f;            // instances per m^2
    int   placement_kind = 0;        // 0=uniform, 1=poisson, 2=cluster
    bool  physics = true;
    float embed = 0.0f;              // physics:false only
    float drop_h[2] = { 0.15f, 0.35f };
    float scale_range[2] = { 1.0f, 1.0f };
    std::string collider_override;   // "" or "auto"|"sphere"|"capsule"|"box"|"hull"
    // Resolved during eval (Task 4). strip[orientation][color]:
    // orientation 0 = vertical strips (column boundaries), 1 = horizontal (row boundaries).
    std::vector<Placement> strip[2][2];
    std::vector<Placement> interior[16];   // tile index = row*4 + col
};

struct DropChildRec { uint64_t child_hash = 0; float transform[16]; };

// Ranges into DslState's op/children arrays emitted inside variant(t) for one tile.
struct VariantRange { int tile = -1; size_t op_begin = 0, op_end = 0, child_begin = 0, child_end = 0; };

struct TilesetSpec {
    TileConfig cfg;
    bool tile_called = false;
    BaseField base;
    std::vector<LayerSpec> layers;
    std::vector<DropChildRec> drops;
    std::vector<VariantRange> variant_ranges;
};

// Mutable recording state attached to DslState during a tileset eval.
struct TilesetState {
    TilesetSpec spec;
    std::string error;               // first tileset-verb error (fail-closed)
    bool has_error = false;
    void set_error(const std::string& m) { if (!has_error) { has_error = true; error = m; } }
    // Per-placement attribute RNG: set/cleared by j_ts_layer for each placement.
    // The params-fn `r` helper reads from this via the native bindings.
    dsl::Rng* param_rng = nullptr;   // non-owning pointer into the current placement loop
};

}  // namespace tileset
