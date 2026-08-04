#pragma once
// lod_distance.h — LOD selection expressed as switch DISTANCES.
//
// This is the M1 representation change of docs/lod-vt-redesign-2026-08-04.md §5:
// one selection function, distance-valued, replacing the several projected-size
// comparisons scattered across the engine. It is introduced as an exact
// re-expression of the existing rule, NOT a behaviour change — see the proof
// below and lod_distance_tests.cpp, which asserts the two agree.
//
// ---------------------------------------------------------------------------
// THE EXISTING RULE (shaders_vk/cull.comp, the draw authority)
//
//     projected_size = bound_radius * instance_scale / distance_to_eye
//                    * pixel_budget * lod_bias
//     lod = the FIRST i (finest, lowest index) with projected_size >= thresholds[i]
//     ...clamped to lod_count-1 when nothing matches.
//
// Thresholds run fine -> coarse, so thresholds[0] is the LARGEST: the finest
// rung demands the biggest on-screen size.
//
// THE SAME RULE IN DISTANCE FORM
//
//     projected_size >= T_i
//   <=> r * s / d * G >= T_i                       (G = pixel_budget * lod_bias)
//   <=> d <= (1 / T_i) * (r * s * G)               (d, r, s, G, T_i all > 0)
//   <=> d <= D_i * reach          where  D_i := 1 / T_i
//                                       reach := bound_radius * scale * G
//
// So the rung whose threshold the projected size clears is exactly the rung
// whose SWITCH DISTANCE the camera is within.
//
// WHY THE RADIUS IS IN `reach` AND NOT IN D_i. Thresholds are radius-relative
// by construction, so D_i normalizes to a UNIT radius as well as unit scale and
// unit dial. That is not a stylistic choice: cull.comp does not always use the
// cluster's baked radius. For a dynamic-bound cluster (an animated part whose
// AABB is unioned per-instance at runtime) it computes
// `local_radius = length(aabb_max - aabb_min) * 0.5` on the GPU, a value that
// does not exist at bake time. Folding a baked radius into D_i would therefore
// have silently mis-selected exactly the dynamic parts. Keeping radius in the
// runtime `reach` handles static and dynamic identically, with no special case
// and no division by a possibly-zero baked radius.
//
// When tables are AUTHORED in metres (M3), the author's `at` is divided by the
// part's radius at bake time to land in this same normalized form, so runtime
// selection never learns the difference.
//
// D_i is INCREASING in i (thresholds decrease), i.e. coarser rungs switch in
// further away, which is the ordering the redesign's authored `at` tables use.
// That is the point of doing this now: once selection reads distances, an
// authored table drops in where a converted one sits, with no other change.
//
// ---------------------------------------------------------------------------
// WHY G IS STILL HERE, when the redesign says the budget leaves the runtime
//
// It leaves once distances are AUTHORED (M3). Until then, removing it would
// change what people see, and M1 is required to be visually inert. Note also
// that pixel_budget is not the simple dial it appears to be: part_flatten.cpp
// bakes it INTO the threshold (`thr0 = 2*radius*pixel_angle*pixel_budget/anchor`)
// while cull.comp multiplies the projected size by it again -- so for parts
// flattened through that path the factor CANCELS and the dial does nothing,
// while for parts using lod_bake.h's hardcoded ladder it works normally.
// Whatever one thinks of that, this header must not change it, and it does not:
// the conversion is applied to whatever threshold was stored, so each part
// keeps the behaviour it already had.
//
// ---------------------------------------------------------------------------
// FLOATING POINT
//
// The two forms are equivalent in exact arithmetic. In floats they can disagree
// for a camera sitting within an ULP or so of a switch boundary, because one
// divides by d and the other by T. That is a tie at a boundary the camera is
// crossing anyway; lod_distance_tests.cpp sweeps the space, asserts agreement,
// and separately asserts every disagreement is boundary-adjacent rather than
// structural.

#include <cmath>
#include <cstddef>

namespace lod {

// Normalized switch distance for one rung: how far a UNIT-radius, unit-scale
// instance can be, at unit global LOD scale, before this rung stops qualifying.
// Multiply by reach() to get the real switch distance in metres.
//
// A threshold of 0 (or negative) means "no lower bound" -- the rung always
// qualifies -- which in distance terms is an infinite switch distance. That
// mirrors the comparison it replaces: projected_size >= 0 holds for every
// positive projected size. A threshold of FLT_MAX (the padding cull.comp's
// unused rung slots carry) maps to ~0, i.e. never qualifies, which is likewise
// what the comparison it replaces does.
inline float normalized_switch_distance(float threshold) noexcept {
    if (!(threshold > 0.0f)) return INFINITY;
    return 1.0f / threshold;
}

// The runtime factor that turns a normalized switch distance into metres.
//
//   bound_radius     : the radius actually in play -- the cluster's baked
//                      radius for a static cluster, or the per-instance
//                      dynamic bound where one exists (see the header note)
//   instance_scale   : the instance's uniform scale
//   global_lod_scale : pixel_budget * lod_bias, the combined runtime dial
inline float reach(float bound_radius, float instance_scale,
                   float global_lod_scale) noexcept {
    return bound_radius * instance_scale * global_lod_scale;
}

// Pick the rung to draw.
//
//   switch_distances : normalized distances, INCREASING, one per rung
//                      (normalized_switch_distance() per threshold)
//   count            : number of rungs; 0 selects rung 0 by convention
//   distance_to_eye  : world distance from camera to the rung's reference point
//   reach_           : reach() above
//
// Returns the FINEST rung the camera is close enough for, clamped to the
// coarsest when it is beyond all of them -- the same clamp cull.comp applies.
inline int select_rep(const float* switch_distances, int count,
                      float distance_to_eye, float reach_) noexcept {
    if (count <= 0 || switch_distances == nullptr) return 0;
    for (int i = 0; i < count; ++i)
        if (distance_to_eye <= switch_distances[i] * reach_) return i;
    return count - 1;
}

} // namespace lod
