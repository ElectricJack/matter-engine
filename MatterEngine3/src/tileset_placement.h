#pragma once
// MatterEngine3/src/tileset_placement.h — deterministic 2D scatter for
// tileset layers.
//
// The tileset DSL's `layer(module, {...})` verb (j_ts_layer in
// dsl_bindings.cpp) fills a LayerSpec by scattering points over 20 fixed
// domains per layer: 4 edge-strip domains (2 orientations x 2 edge colours)
// and 16 interior domains, one per tile of the 4x4 Wang torus. This file is
// the point generator for all of them and knows nothing about tiles, parts or
// physics.
//
// Coordinates and units
//   - All coordinates are DOMAIN-local metres in the XZ plane. Y never
//     appears here; the caller assigns drop height / embed depth afterwards.
//   - `density` is instances per square metre. The target count is
//     round(density * rect area) -- the clear disks are NOT subtracted from
//     that area, they are handled by rejection, so a domain with large disks
//     yields fewer points than its density implies.
//
// Determinism (the reason this is not rand() or std::mt19937)
//   Every draw comes from dsl::Rng (SplitMix64, dsl_rng.h) seeded through
//   placement_seed(). The same master seed, layer index, domain id, domain
//   rect and kind produce byte-identical output on every machine and every
//   run, which is what keeps a tileset's baked layout reproducible.
//
// Gotcha: every scatter kind is rejection-based with a bounded attempt
// budget, so the returned vector can be SHORTER than the requested target and
// callers must not assume a count. Poisson and Cluster undershoot the most
// (dart throwing against a min-distance radius; gaussian offsets that land
// outside the rect are dropped).

#include <cstdint>
#include <vector>

namespace tileset {

// One scattered point in the domain's local XZ frame, metres. Y is the
// caller's business (drop height for physics layers, embed depth otherwise).
struct Point2 { float x, z; };   // domain-local

// Which pattern `scatter()` uses. The integer values are part of the script
// ABI -- LayerSpec::placement_kind carries them straight through from the JS
// layer spec and is cast to this enum -- so do not renumber them.
//   Uniform - independent uniform samples over the rect; reaches the target
//             count unless the clear disks reject too much.
//   Poisson - dart throwing with a minimum separation of 0.7/sqrt(density)
//             metres; evenly spread, and the kind most likely to return
//             fewer points than requested.
//   Cluster - target/8+1 uniform cluster centres with gaussian offsets
//             (sigma 0.15 m); clumpy, and offsets outside the rect are
//             discarded rather than clamped.
enum class PlacementKind { Uniform = 0, Poisson = 1, Cluster = 2 };

// Rectangular domain [x0,x1) x [z0,z1) with corner-clear disks.
// Domain-local metres. An edge-strip domain is a thin rect straddling a tile
// boundary (x in [-w, +w), z in [0, tile size)) carrying a clear disk at each
// end so no instance sits on a tile corner; an interior domain is the tile
// inset by the strip width, with no disks. See j_ts_layer in dsl_bindings.cpp
// for the exact 20-domain construction.
struct PlacementDomain {
    float x0, x1, z0, z1;
    // Disk centers (domain-local) that placements must clear by `clear_radius`.
    std::vector<Point2> clear_disks;
    float clear_radius = 0.0f;
};

// Deterministic scatter: expected count = density * usable area (rect area; the
// clear disks are handled by rejection). Same seed + domain + kind => same output.
// Returns fewer points than that expectation whenever rejection or the
// per-kind attempt budget runs out, and an empty vector when the rounded
// target is <= 0 (a tiny domain or a tiny density). Both are normal outcomes,
// not errors -- there is no failure channel.
std::vector<Point2> scatter(PlacementKind kind, const PlacementDomain& dom,
                            float density, uint64_t seed);

// Seed folding used by every placement call site (documented, test-guarded):
// fold(master, layer_index, domain_id) with SplitMix64 avalanche per fold.
// `master` is TileConfig::seed; `layer_index` is the layer's position in
// TilesetSpec::layers; `domain_id` is orient*2+color for the four strip
// domains and 4+tile for the sixteen interiors. Both indices are folded as
// (index + 1) so that layer 0 and domain 0 still perturb the master seed.
// Per-placement attribute RNGs are derived from the result by the caller
// (dom_seed ^ 0xA5A5...), so adding an attribute draw cannot move positions.
uint64_t placement_seed(uint64_t master, uint32_t layer_index, uint32_t domain_id);

} // namespace tileset
