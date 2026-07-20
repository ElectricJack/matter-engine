#include "selection_bounds.h"

#include <algorithm>
#include <cstring>

#include "matter/ecs.h"
#include "matter/query.h"
#include "matter/scene.h"
#include "matter/world_session.h"

namespace viewer {

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

bool bounds_for_object(const SelectedObject& obj, matter::WorldSession& session,
                       SelectionBounds& out) {
    if (obj.kind == SelectedObject::BakedRoot) {
        const uint32_t count = session.instance_count();
        for (uint32_t i = 0; i < count; ++i) {
            matter::InstanceInfo info;
            if (!session.instance_info(i, info)) continue;
            if (info.part_hash != obj.id) continue;
            std::copy(info.transform, info.transform + 16, out.world_matrix);
            local_aabb_for_part(session, info.part_hash, 2.0f, out.local_min, out.local_max);
            return true;
        }
        return false;
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
