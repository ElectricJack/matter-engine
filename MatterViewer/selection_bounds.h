#pragma once
#include "selection_set.h"
#include <cstdint>

namespace matter { class WorldSession; }

namespace viewer {

struct SelectionBounds {
    float local_min[3];
    float local_max[3];
    float world_matrix[16];
};

// Compute the local-space AABB for a given part_hash.
// Falls back to ±default_half if no geometry clusters are loaded.
// Shared by both outline rendering and pick raycasting.
void local_aabb_for_part(matter::WorldSession& session, uint64_t part_hash,
                         float default_half, float out_min[3], float out_max[3]);

// Compute the local-space AABB and world transform for a selected object.
// Both the selection outline renderer and the viewport pick raycast use this
// so they always agree on geometry.
bool bounds_for_object(const SelectedObject& obj, matter::WorldSession& session,
                       SelectionBounds& out);

} // namespace viewer
