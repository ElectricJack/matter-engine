#pragma once

// MatterEditor/src/selection_bounds.h
//
// The geometry behind a selection: local-space AABB + world placement for a
// `SelectedObject`. Deliberately one implementation for two consumers — the
// selection outline overlay (selection_outline.cpp) and the viewport pick
// raycast (viewport_pick.cpp) — so the box the user sees and the box the click
// tests are the same box by construction rather than by two matching edits.
//
// Units are world metres throughout. Reads the live WorldSession, so app
// thread only. Implementation in selection_bounds.cpp.

#include "selection_set.h"
#include <cstdint>

namespace matter { class WorldSession; }

namespace viewer {

// An oriented box: an axis-aligned extent in the object's LOCAL space, plus
// the matrix that places that space in the world. Deliberately not a world
// AABB — a rotated part's world AABB would be a much looser box than the
// outline the user expects.
//
// No default member initializers: a SelectionBounds is garbage until
// `bounds_for_object` has returned TRUE for it. Never read the fields after a
// false return.
struct SelectionBounds {
    float local_min[3];   // local-space corner, metres
    float local_max[3];   // local-space corner, metres
    // Local -> world, 16 floats, ROW-major: translation lives in elements 3, 7
    // and 11. That is how selection_outline.cpp's transform_point reads it and
    // how the LocalTransform fallback in selection_bounds.cpp writes it. Note
    // this is the opposite convention from the column-major view/projection
    // Mat4 in selection_outline.cpp — the two never meet, but do not copy one
    // into the other.
    float world_matrix[16];
};

// Compute the local-space AABB for a given part_hash.
// Falls back to ±default_half if no geometry clusters are loaded.
// Shared by both outline rendering and pick raycasting.
// `default_half` is a cube half-extent in metres; a `part_hash` of 0 (an
// entity with no PartInstance) always takes the fallback. Always writes all
// six floats, so there is no failure signal to check.
void local_aabb_for_part(matter::WorldSession& session, uint64_t part_hash,
                         float default_half, float out_min[3], float out_max[3]);

// Compute the local-space AABB and world transform for a selected object.
// Both the selection outline renderer and the viewport pick raycast use this
// so they always agree on geometry.
//
// Returns false and leaves `out` untouched when the object no longer resolves
// (a baked-root hash with no instance in this world, or an entity id gone from
// the ECS) — a normal outcome, not an error. The Entity path scans the whole
// ECS, so this is O(entities) per call.
bool bounds_for_object(const SelectedObject& obj, matter::WorldSession& session,
                       SelectionBounds& out);

} // namespace viewer
