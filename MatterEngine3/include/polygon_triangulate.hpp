#pragma once
// Phase 3: a constrained ear-clipping triangulator that supports holes.
//
// Pure, dependency-free (no MSL/raylib types) so it is unit-testable in
// isolation and shared by three callers: POLYGON flat fills, extrude end caps,
// and (later) the disc/annulus caps of the round primitives.
//
// Input: a Profile = one outer contour (CCW) plus zero or more hole contours
// (CW). Holes are bridged into the outer loop (the standard earcut approach),
// then the merged simple polygon is ear-clipped.
//
// Output: a flat triangle index list referencing the concatenated vertex list
// [outer..., hole0..., hole1..., ...] in that exact order. Each group of three
// indices is one triangle, wound CCW (matching the outer contour) so the filled
// face / cap faces +normal of the profile plane.
#include <vector>

namespace poly_tri {

// 2D point in the cross-section (u,v) plane.
struct P2 { float x, y; };

using Contour = std::vector<P2>;

struct Profile {
    Contour              outer;   // CCW outer boundary
    std::vector<Contour> holes;   // each CW (wound opposite the outer)
};

// Triangulate the profile. Returns indices into the concatenated vertex list
// (outer first, then holes in order). Size is a multiple of 3. Returns empty on
// a degenerate/too-small profile.
std::vector<int> triangulate(const Profile& profile);

} // namespace poly_tri
