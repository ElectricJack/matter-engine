#pragma once
// MatterEngine3/src/tileset_spec.h — the recorded form of a tileset script.
//
// A tileset root module is a JS script whose verbs (tile, base, layer,
// dropChild, variant) do not build geometry directly: they RECORD into the
// structs below. dsl_bindings.cpp and script_host.cpp own the recording side;
// tileset_bake.cpp consumes the finished TilesetSpec and settles it into a
// SettledTorus (tileset_bake.h), and the .gtex atlas bake reads the same data.
//
// The world is a 4x4 de Bruijn torus of Wang tiles (tileset_layout.h): 16
// tiles, 2 edge colours per orientation, wrapping in x and z. That is why the
// fixed array sizes below are 2, 2 and 16 -- they are the tile set itself, not
// a tuning parameter.
//
// Units and spaces
//   - Lengths are metres unless stated otherwise; `size` is one tile edge.
//   - Placement coordinates are DOMAIN-local (see Placement), not torus or
//     world space; tileset_bake.cpp maps them onto the torus.
//   - Quaternions are xyzw.
//
// Determinism: everything here derives from TileConfig::seed plus the script
// text, with no wall-clock or process entropy, so the same script and seed
// produce the same spec on every machine. TilesetState is the mutable scratch
// that exists only for the duration of one eval; TilesetSpec is the durable
// result.

#include <cstdint>
#include <string>
#include <vector>
#include "dsl_rng.h"

namespace tileset {

// Whole-tileset settings, recorded by the script's tile() verb. `seed` is the
// master seed every placement RNG folds (tileset_placement.h), so changing it
// re-rolls the entire tileset. `texels_per_meter` sizes the baked .gtex atlas.
// `edge_strip_width` is the half-width of the band straddling each tile
// boundary and `corner_clear_radius` the keep-out disk at each strip end, both
// in metres; tile() enforces edge_strip_width > corner_clear_radius so nothing
// straddles a tile corner.
struct TileConfig {
    float    size = 2.0f;               // meters per tile edge
    int      texels_per_meter = 512;
    uint64_t seed = 0;
    float    edge_strip_width = 0.15f;  // m
    float    corner_clear_radius = 0.08f;
};

// base(fn, material): heightfield sampled during eval on a per-tile periodic grid.
// heights[z*n + x] = fn(x*cell, z*cell); the grid tiles toroidally (sample n wraps to 0).
// Only ONE tile is stored; consumers repeat it across the 4x4 torus
// themselves (build_base_blas in tileset_torus_bvh.cpp tessellates kTorusN*n
// samples per side out of it, and the settle pass builds its HeightField the
// same way). `set` false means the script never called base(); build_base_blas
// rejects such a spec rather than treating it as flat ground.
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
// child_hash is the resolved part hash (module plus canonical params) that the
// parts cache is keyed by, and `scale` is uniform.
struct Placement {
    uint64_t child_hash = 0;
    float pos[3]  = { 0, 0, 0 };
    float quat[4] = { 0, 0, 0, 1 };  // xyzw
    float scale   = 1.0f;
};

// One layer(module, {...}) call: a part scattered over the torus. The scatter
// runs at EVAL time, not bake time, so the arrays at the bottom already hold
// resolved point lists rather than parameters.
//
// physics true drops the instances into the settle world, spawning them at a
// random height in the drop_h [min, max] range (metres) above the base;
// physics false snaps them analytically onto the base with `embed` metres of
// sink and never touches box3d.
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
    // color is the boundary edge colour 0/1 (tileset_layout.h). These four
    // strip domains plus the sixteen interior domains are the twenty scatter
    // domains of one layer; their seeds are placement_seed(cfg.seed, layer
    // index, orient*2 + color) and placement_seed(..., 4 + tile).
    std::vector<Placement> strip[2][2];
    std::vector<Placement> interior[16];   // tile index = row*4 + col
};

// A dropChild() call: one part placed by the DSL transform stack instead of by
// scatter. `transform` is a snapshot of the stack top as a ROW-major 4x4
// (mm::Mat4 layout, copied element for element in dsl_bindings.cpp). It has no
// default member initializer -- the verb value-initializes the record before
// filling it in. Drops are shared across the torus: the bake replicates each
// one onto all 16 tiles, ahead of any layer.
struct DropChildRec { uint64_t child_hash = 0; float transform[16]; };

// Ranges into DslState's op/children arrays emitted inside variant(t) for one tile.
struct VariantRange { int tile = -1; size_t op_begin = 0, op_end = 0, child_begin = 0, child_end = 0; };

// The complete recording of one tileset script eval -- the hand-off from the
// script host to tileset_bake.cpp. `tile_called` records whether the script
// ever called tile(), i.e. whether `cfg` holds script-provided values or is
// still the defaults declared in TileConfig.
struct TilesetSpec {
    TileConfig cfg;
    bool tile_called = false;
    BaseField base;
    std::vector<LayerSpec> layers;
    std::vector<DropChildRec> drops;
    std::vector<VariantRange> variant_ranges;
};

// Mutable recording state attached to DslState during a tileset eval.
// It lives only for the duration of that eval and is owned by the script host;
// the tileset verbs in dsl_bindings.cpp reach it through the DslState. Errors
// are fail-closed and first-wins -- set_error keeps the FIRST message and
// nothing aborts the JS evaluation, so the caller must check has_error once
// the eval returns.
struct TilesetState {
    TilesetSpec spec;
    std::string error;               // first tileset-verb error (fail-closed)
    bool has_error = false;
    void set_error(const std::string& m) { if (!has_error) { has_error = true; error = m; } }
    // Per-placement attribute RNG: set/cleared by j_ts_layer for each placement.
    // The params-fn `r` helper reads from this via the native bindings.
    dsl::Rng* param_rng = nullptr;   // non-owning pointer into the current placement loop

    // variant() registration (Task 5).
    // The JS function value is stored as raw bits (16 bytes = sizeof(JSValue) on the
    // target platform: 4-byte union + 8-byte int64 tag). Stored via memcpy so this
    // header need not include quickjs.h. Set/read only from dsl_bindings.cpp /
    // script_host.cpp which include quickjs.h and know the layout.
    bool variant_called = false;     // variant() was invoked at least once
    bool variant_fn_set = false;     // a valid fn was stored in variant_fn_bits
    uint64_t variant_fn_bits[2] = { 0, 0 };  // memcpy of the duped JSValue
};

}  // namespace tileset
