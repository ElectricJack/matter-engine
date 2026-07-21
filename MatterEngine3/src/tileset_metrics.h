#pragma once
// tileset_metrics.h — pose-delta metric and acceptance gate for settle
// optimization experiments (settle-tick-optimizer.md §II.3).
//
// Decides whether a candidate settle run is "visually close" to a baseline
// run of the same spec + seed: position deltas normalized by each instance's
// collider characteristic size, orientation deltas with symmetric-shape
// leniency, gated on p95 + teleport count. This is NOT a determinism check —
// that remains pose_hash.
//
// Characteristic-size input: SettledTorus stores no collider info (and the
// settle-cache format must stay untouched), so compare_settled takes a
// caller-built map child_hash -> PoseMetricInfo. Callers build it from the
// SettlePlan's colliders via metric_info_of() so the size/weight rules live
// here, not at every call site. Hashes absent from the map fall back to a
// 0.5 m characteristic size with full orientation weight. The map is keyed
// per child hash (not per scale): the char size is the fit's size as-is; if
// per-scale precision matters, callers can pre-bake scale into the value.

#include "tileset_bake.h"      // SettledTorus, SettledInstance
#include "tileset_collider.h"  // ColliderFit, ColliderType
#include <cstdint>
#include <map>

namespace tileset {

// Advisory acceptance gate (tunable constants in one place; enforcement
// lives in whatever test/tool adopts it). pass requires p95 <= gate.p95,
// teleports <= gate.teleports, and p95_orient_deg <= gate.p95_orient_deg.
struct PoseDeltaGate {
    float p95 = 0.15f;             // normalized position delta
    int   teleports = 0;           // allowed count of normalized delta > 1.0
    float p95_orient_deg = 15.0f;  // weighted orientation delta, degrees
};

struct PoseDeltaReport {
    float median = 0, p95 = 0, max = 0;  // normalized position delta
    float p95_orient_deg = 0;            // over pairs with orient_weight > 0
    int   teleports = 0;                 // normalized delta > 1.0
    int   compared = 0, skipped = 0;     // skipped = count-mismatch guard
    bool  pass = false;                  // vs. PoseDeltaGate (see above)
};

// Per-child-hash metric parameters.
struct PoseMetricInfo {
    float char_size = 0.5f;      // meters; normalizes position deltas
    float orient_weight = 1.0f;  // 0 = orientation skipped (Sphere),
                                 // 0.5 = near-isotropic, 1 = full weight
};
using PoseMetricMap = std::map<uint64_t, PoseMetricInfo>;

// Characteristic size of a fitted collider: radius for Sphere/Capsule,
// max half_extent for Box, bounding radius (max |point - center|) for Hull.
// Non-positive results fall back to 0.5 m.
float char_size_of(const ColliderFit& fit);

// Orientation weight: 0 for Sphere (rotation invisible), 0.5 when the
// shape's extent ratio (longest/shortest) is < 1.2 (near-isotropic), 1
// otherwise. Capsule extents are {seg_half + radius, radius, radius};
// Box/Hull use half_extent[0..2].
float orient_weight_of(const ColliderFit& fit);

// Convenience: both rules in one PoseMetricMap value.
PoseMetricInfo metric_info_of(const ColliderFit& fit);

// Compare two settle runs of the same spec/seed. Instances pair by index
// (spawn order is deterministic per spec). An instance-count mismatch fails
// closed: the common prefix is still compared, all unpaired instances are
// counted in `skipped`, and `pass` is forced false.
//
// Position deltas take the shortest path modulo the torus extent
// (kTorusN * baseline.cfg.size) in x/z, matching sync_groups_step's
// half-torus logic; extent <= 0 disables wrapping. Each delta is divided by
// the pair's characteristic size (looked up by the BASELINE instance's
// child_hash; 0.5 m fallback).
//
// Orientation delta is the quaternion angle 2*acos(|dot|) in degrees,
// multiplied by orient_weight; pairs with weight 0 contribute no
// orientation sample.
//
// median/p95 are nearest-rank percentiles (sorted values, index
// ceil(q*N) - 1); an empty sample set reports 0.
PoseDeltaReport compare_settled(const SettledTorus& baseline,
                                const SettledTorus& candidate,
                                const PoseMetricMap& metric_info = {},
                                const PoseDeltaGate& gate = {});

} // namespace tileset
