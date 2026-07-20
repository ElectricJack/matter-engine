#include "viewport_pick.h"
#include "selection_bounds.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

bool invert_affine(const float m[16], float inv[16]) {
    float a[9] = {m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10]};
    float det = a[0]*(a[4]*a[8]-a[5]*a[7])
              - a[1]*(a[3]*a[8]-a[5]*a[6])
              + a[2]*(a[3]*a[7]-a[4]*a[6]);
    if (std::abs(det) < 1e-20f) return false;
    float inv_det = 1.0f / det;
    float r[9];
    r[0] =  (a[4]*a[8]-a[5]*a[7]) * inv_det;
    r[1] = -(a[1]*a[8]-a[2]*a[7]) * inv_det;
    r[2] =  (a[1]*a[5]-a[2]*a[4]) * inv_det;
    r[3] = -(a[3]*a[8]-a[5]*a[6]) * inv_det;
    r[4] =  (a[0]*a[8]-a[2]*a[6]) * inv_det;
    r[5] = -(a[0]*a[5]-a[2]*a[3]) * inv_det;
    r[6] =  (a[3]*a[7]-a[4]*a[6]) * inv_det;
    r[7] = -(a[0]*a[7]-a[1]*a[6]) * inv_det;
    r[8] =  (a[0]*a[4]-a[1]*a[3]) * inv_det;
    float tx = m[3], ty = m[7], tz = m[11];
    std::memset(inv, 0, sizeof(float)*16);
    inv[0]=r[0]; inv[1]=r[1]; inv[2]=r[2];
    inv[4]=r[3]; inv[5]=r[4]; inv[6]=r[5];
    inv[8]=r[6]; inv[9]=r[7]; inv[10]=r[8];
    inv[3]  = -(r[0]*tx + r[1]*ty + r[2]*tz);
    inv[7]  = -(r[3]*tx + r[4]*ty + r[5]*tz);
    inv[11] = -(r[6]*tx + r[7]*ty + r[8]*tz);
    inv[15] = 1.0f;
    return true;
}

void xform_point(const float m[16], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[1]*in[1] + m[2]*in[2]  + m[3];
    out[1] = m[4]*in[0] + m[5]*in[1] + m[6]*in[2]  + m[7];
    out[2] = m[8]*in[0] + m[9]*in[1] + m[10]*in[2] + m[11];
}

void xform_dir(const float m[16], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[1]*in[1] + m[2]*in[2];
    out[1] = m[4]*in[0] + m[5]*in[1] + m[6]*in[2];
    out[2] = m[8]*in[0] + m[9]*in[1] + m[10]*in[2];
}

bool ray_obb(const float origin[3], const float dir[3],
             const float world_mat[16],
             const float local_min[3], const float local_max[3],
             float& t_out) {
    float inv[16];
    if (!invert_affine(world_mat, inv)) return false;
    float local_origin[3], local_dir[3];
    xform_point(inv, origin, local_origin);
    xform_dir(inv, dir, local_dir);
    return ray_aabb(local_origin, local_dir, local_min, local_max, t_out);
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

        float local_min[3], local_max[3];
        local_aabb_for_part(session, info.part_hash, 2.0f, local_min, local_max);

        float t = 0.0f;
        if (ray_obb(origin, ray_dir, info.transform, local_min, local_max, t) &&
            t < best_t) {
            best_t = t;
            found = true;
            best_object.kind = SelectedObject::BakedRoot;
            best_object.id = info.part_hash;
        }
    }

    session.ecs().each(
        [&](flecs::entity e, const matter::scene::SceneEntityId& sid,
            const matter::ecs::LocalTransform& lt) {
            float mat[16];
            if (e.has<matter::ecs::WorldTransform>()) {
                auto wt = e.get<matter::ecs::WorldTransform>();
                std::copy(wt.matrix.m, wt.matrix.m + 16, mat);
            } else {
                std::fill(mat, mat + 16, 0.0f);
                mat[0] = lt.scale.x; mat[5] = lt.scale.y; mat[10] = lt.scale.z;
                mat[3] = lt.translation.x; mat[7] = lt.translation.y;
                mat[11] = lt.translation.z; mat[15] = 1.0f;
            }

            uint64_t part_hash = 0;
            if (e.has<matter::scene::PartInstance>()) {
                auto pi = e.get<matter::scene::PartInstance>();
                part_hash = pi.part_hash;
            }
            float local_min[3], local_max[3];
            local_aabb_for_part(session, part_hash, 0.5f, local_min, local_max);

            float t = 0.0f;
            if (ray_obb(origin, ray_dir, mat, local_min, local_max, t) &&
                t < best_t) {
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
