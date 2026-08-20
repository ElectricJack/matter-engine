// MatterEditor/src/selection_bounds.cpp
//
// Turns a `SelectedObject` into geometry: a local-space AABB plus the world
// matrix that places it. This is the single shared implementation behind the
// selection outline overlay (selection_outline.cpp) and the viewport pick
// raycast (viewport_pick.cpp), which is what keeps "what the box drew" and
// "what the click hit" from drifting apart.
//
// Two lookup paths, because the two selectable kinds live in different places:
//
//  - BakedRoot — `WorldSession::instance_info_by_hash` gives the placed
//    instance's transform and the part hash behind it; the AABB then comes
//    from the part.
//  - Entity — a linear walk of the ECS for the row carrying the matching
//    SceneEntityId. Its world matrix is the `WorldTransform` component when
//    present, and otherwise a scale+translate matrix synthesised from
//    `LocalTransform` (rotation is DROPPED in that fallback). The AABB comes
//    from the entity's `PartInstance` part hash, or from the default extent
//    when it has none.
//
// Everything is in world units (metres). The world matrix is row-major with
// translation in elements 3, 7 and 11 — see SelectionBounds in the header.
//
// Reads the live session, so app thread only, and not while the session is
// being replaced.

#include "selection_bounds.h"

#include <algorithm>
#include <cstring>

#include "matter/ecs.h"
#include "matter/query.h"
#include "matter/scene.h"
#include "matter/world_session.h"

namespace viewer {

// `default_half` is a fallback CUBE half-extent in metres, used whenever the
// session cannot answer for `part_hash` — an unbaked or unloaded part, or the
// `part_hash == 0` an entity with no PartInstance produces. Callers pass
// different defaults deliberately: 2.0 m for a baked root (bounds_for_object
// below) and 0.5 m for an entity, since a bare entity marker should be small
// and a whole baked part should not be. Always writes all six output floats.
void local_aabb_for_part(matter::WorldSession& session, uint64_t part_hash,
                         float default_half, float out_min[3], float out_max[3]) {
    matter::PartBounds bounds;
    if (session.part_bounds(part_hash, bounds)) {
        std::copy(bounds.aabb_min, bounds.aabb_min + 3, out_min);
        std::copy(bounds.aabb_max, bounds.aabb_max + 3, out_max);
    } else {
        for (int a = 0; a < 3; ++a) {
            out_min[a] = -default_half;
            out_max[a] =  default_half;
        }
    }
}

// Returns false — leaving `out` COMPLETELY UNTOUCHED, and SelectionBounds has
// no default member initializers — when the object no longer resolves: a baked
// root whose hash is not instanced in this world, or an entity id no longer in
// the ECS. Callers must therefore test the return before reading `out`. A
// false here is a normal outcome, not an error: it is exactly what happens for
// one frame after content is regenerated but before SelectionSet::validate
// prunes the stale entry.
//
// The Entity path is a full ECS scan per call: flecs `each` cannot break, so
// the `found` flag short-circuits the body but not the iteration. That makes
// this O(entities) for every selected object, every frame it is drawn — fine
// for a handful of selected items, not something to call in a loop over the
// scene.
bool bounds_for_object(const SelectedObject& obj, matter::WorldSession& session,
                       SelectionBounds& out) {
    if (obj.kind == SelectedObject::BakedRoot) {
        matter::InstanceInfo info;
        if (!session.instance_info_by_hash(obj.id, info)) return false;
        std::copy(info.transform, info.transform + 16, out.world_matrix);
        local_aabb_for_part(session, info.part_hash, 2.0f, out.local_min, out.local_max);
        return true;
    }

    bool found = false;
    session.ecs().each(
        [&](flecs::entity e, const matter::scene::SceneEntityId& sid,
            const matter::ecs::LocalTransform& lt) {
            if (found || sid.value != obj.id) return;
            found = true;

            if (e.has<matter::ecs::WorldTransform>()) {
                auto wt = e.get<matter::ecs::WorldTransform>();
                std::copy(wt.matrix.m, wt.matrix.m + 16, out.world_matrix);
            } else {
                std::fill(out.world_matrix, out.world_matrix + 16, 0.0f);
                out.world_matrix[0] = lt.scale.x;
                out.world_matrix[5] = lt.scale.y;
                out.world_matrix[10] = lt.scale.z;
                out.world_matrix[3] = lt.translation.x;
                out.world_matrix[7] = lt.translation.y;
                out.world_matrix[11] = lt.translation.z;
                out.world_matrix[15] = 1.0f;
            }

            uint64_t part_hash = 0;
            if (e.has<matter::scene::PartInstance>()) {
                auto pi = e.get<matter::scene::PartInstance>();
                part_hash = pi.part_hash;
            }
            local_aabb_for_part(session, part_hash, 0.5f, out.local_min, out.local_max);
        });
    return found;
}

} // namespace viewer
