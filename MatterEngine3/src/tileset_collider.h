#pragma once
// tileset_collider.h — fitting a cheap collision proxy to a vertex cloud.
//
// WHAT THIS IS. One function: given a part's vertices, choose a primitive
// (sphere / capsule / box / crude hull) that approximates it and return the
// primitive's parameters in a `ColliderFit`. Pure geometry -- no physics
// engine, no engine headers, no I/O.
//
// WHERE IT SITS. `tileset_part_collider.cpp` loads a baked child part's
// vertices and calls this; `tileset_bake.cpp` memoizes the result per
// (child_hash, collider_override), derives per-scale copies with `scale_fit`,
// and hands pointers to them to the box3d settle world. So one call here is
// amortised across every instance of that child in the 4x4 torus.
//
// SPACES AND UNITS. Metres, in whatever space the caller's vertices are in
// (part-local for the tileset path). `center` is in that same space;
// `axis[k]` are unit row vectors forming an orthonormal frame sorted by
// DESCENDING extent, so `axis[0]` is always the longest direction and
// `half_extent[0] >= half_extent[1] >= half_extent[2]`.
//
// SHARP EDGES.
//   * `hull_points` IS NOT A CONVEX HULL. It is a stride-decimated subsample of
//     the input cloud, capped at 64 points, so it can omit an extreme vertex
//     and under-cover the shape. It is only meaningful for `Hull`.
//   * `volume` is analytic and correct for Sphere/Capsule/Box, but for Hull it
//     is a heuristic (~half the OBB); box3d computes true mass from the shape
//     it is given, so treat this field as a sorting/heuristic value only.
//   * Fields not relevant to the chosen `type` keep their defaults --
//     `radius`/`seg_half` are 0 for Box and Hull. Always branch on `type`.
//     `center`, `axis` and `half_extent` are filled for every type.
//   * Because these fits feed the settle simulation, changing the fitting rule
//     changes settled poses for existing content -- a cache-invalidating
//     change, not a cosmetic one.
#include <cstddef>
#include <vector>

namespace tileset {

// Which primitive the fit chose, and therefore which `ColliderFit` fields carry
// meaning:
//   Sphere   `center` + `radius`.
//   Capsule  `center` + `radius` + `seg_half`, core segment along `axis[0]`.
//   Box      `center` + `axis` + `half_extent`.
//   Hull     `hull_points` (a subsample -- see the header note). Also the
//            fallback for an unrecognised override string.
enum class ColliderType { Sphere, Capsule, Box, Hull };

// A fitted collision proxy. Plain value type: copyable, movable, owns nothing
// but its own `hull_points`, and holds no reference back to the cloud it was
// fitted from. Every field is in the input cloud's space and in metres.
//
// The PCA frame (`center`, `axis`, `half_extent`) is filled regardless of
// `type`, so it stays usable as a bounding box even for a sphere fit; the
// type-specific fields are the ones to branch on. `scale_fit` in
// tileset_part_collider.h produces a uniformly scaled copy -- lengths scale
// linearly and `volume` cubically -- which is how one fit serves every scale
// variant of a child.
struct ColliderFit {
    ColliderType type = ColliderType::Hull;
    float center[3] = { 0, 0, 0 };
    float axis[3][3] = { { 1,0,0 }, { 0,1,0 }, { 0,0,1 } };  // orthonormal, desc. extent
    float half_extent[3] = { 0, 0, 0 };                      // along axis[0..2]
    float radius = 0.0f;      // Sphere / Capsule
    float seg_half = 0.0f;    // Capsule core segment half-length along axis[0]
    std::vector<float> hull_points;  // Hull: xyz triples, <= 64 points
    float volume = 0.0f;      // analytic volume of the fitted primitive
};

// Fit a collision proxy to a vertex cloud (xyz triples) via PCA-OBB and an
// aspect-ratio heuristic. override_kind: nullptr/"auto" for the heuristic,
// or "sphere" | "capsule" | "box" | "hull" to force a type.
ColliderFit fit_collider(const float* xyz, size_t vertex_count,
                         const char* override_kind = nullptr);

} // namespace tileset
