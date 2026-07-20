#include "viewport_pick.h"

#include <algorithm>
#include <cmath>

#include "matter/ecs.h"
#include "matter/query.h"
#include "matter/scene.h"
#include "matter/world_session.h"

namespace viewer {

namespace {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 normalize(const Vec3& a) {
    const float len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (len < 1e-8f) return {0.0f, 0.0f, 1.0f};
    return {a.x / len, a.y / len, a.z / len};
}

bool ray_aabb(const float origin[3], const float dir[3],
              const float aabb_min[3], const float aabb_max[3],
              float& t_out) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8f) {
            if (origin[i] < aabb_min[i] || origin[i] > aabb_max[i]) return false;
        } else {
            const float inv_d = 1.0f / dir[i];
            float t1 = (aabb_min[i] - origin[i]) * inv_d;
            float t2 = (aabb_max[i] - origin[i]) * inv_d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    t_out = tmin;
    return true;
}

}  // namespace

PickResult viewport_pick(float cursor_x, float cursor_y,
                         int fb_width, int fb_height,
                         const matter::CameraDesc& camera,
                         matter::WorldSession& session) {
    PickResult result;
    if (fb_width <= 0 || fb_height <= 0) return result;

    const float ndc_x = (2.0f * cursor_x / static_cast<float>(fb_width)) - 1.0f;
    const float ndc_y = 1.0f - (2.0f * cursor_y / static_cast<float>(fb_height));

    const Vec3 position{camera.position.x, camera.position.y, camera.position.z};
    const Vec3 target{camera.target.x, camera.target.y, camera.target.z};
    const Vec3 up{camera.up.x, camera.up.y, camera.up.z};

    const Vec3 forward = normalize(sub(target, position));
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 cam_up = cross(right, forward);

    const float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);
    const float half_height = std::tan(camera.vertical_fov_radians * 0.5f);
    const float half_width = half_height * aspect;

    const Vec3 dir = normalize(add(
        add(forward, scale(right, ndc_x * half_width)),
        scale(cam_up, ndc_y * half_height)));

    const float origin[3] = {position.x, position.y, position.z};
    const float ray_dir[3] = {dir.x, dir.y, dir.z};

    float best_t = 1e30f;
    bool found = false;
    SelectedObject best_object;

    const uint32_t instance_count = session.instance_count();
    for (uint32_t i = 0; i < instance_count; ++i) {
        matter::InstanceInfo info;
        if (!session.instance_info(i, info)) continue;
        const float translation[3] = {info.transform[3], info.transform[7],
                                       info.transform[11]};
        constexpr float kHalfExtent = 2.0f;
        const float aabb_min[3] = {translation[0] - kHalfExtent,
                                    translation[1] - kHalfExtent,
                                    translation[2] - kHalfExtent};
        const float aabb_max[3] = {translation[0] + kHalfExtent,
                                    translation[1] + kHalfExtent,
                                    translation[2] + kHalfExtent};
        float t = 0.0f;
        if (ray_aabb(origin, ray_dir, aabb_min, aabb_max, t) && t < best_t) {
            best_t = t;
            found = true;
            best_object.kind = SelectedObject::BakedRoot;
            best_object.id = info.part_hash;
        }
    }

    // Entities are identified by their stable SceneEntityId (authored-id
    // hash), NOT the flecs entity id — the whole editor (scene tree,
    // properties, gizmo, field commands, per-frame selection validation)
    // resolves SelectedObject::Entity ids in SceneEntityId space.
    session.ecs().each(
        [&](flecs::entity, const matter::scene::SceneEntityId& sid,
            const matter::ecs::LocalTransform& lt) {
            constexpr float kHalfExtent = 0.5f;
            const float aabb_min[3] = {lt.translation.x - kHalfExtent,
                                        lt.translation.y - kHalfExtent,
                                        lt.translation.z - kHalfExtent};
            const float aabb_max[3] = {lt.translation.x + kHalfExtent,
                                        lt.translation.y + kHalfExtent,
                                        lt.translation.z + kHalfExtent};
            float t = 0.0f;
            if (ray_aabb(origin, ray_dir, aabb_min, aabb_max, t) && t < best_t) {
                best_t = t;
                found = true;
                best_object.kind = SelectedObject::Entity;
                best_object.id = sid.value;
            }
        });

    result.hit = found;
    result.object = best_object;
    result.distance = found ? best_t : 0.0f;
    return result;
}

} // namespace viewer
