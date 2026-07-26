#include "camera_focus.h"

#include <algorithm>
#include <cmath>

namespace viewer {
namespace {

struct Aabb {
    matter::Float3 min{};
    matter::Float3 max{};
    bool valid = false;
};

void expand(Aabb& box, const matter::Float3& center, float half_extent) {
    const matter::Float3 lo{center.x - half_extent, center.y - half_extent,
                            center.z - half_extent};
    const matter::Float3 hi{center.x + half_extent, center.y + half_extent,
                            center.z + half_extent};
    if (!box.valid) {
        box.min = lo;
        box.max = hi;
        box.valid = true;
        return;
    }
    box.min.x = std::min(box.min.x, lo.x);
    box.min.y = std::min(box.min.y, lo.y);
    box.min.z = std::min(box.min.z, lo.z);
    box.max.x = std::max(box.max.x, hi.x);
    box.max.y = std::max(box.max.y, hi.y);
    box.max.z = std::max(box.max.z, hi.z);
}

// Fixed framing FOV independent of the live camera's own vertical_fov_radians
// (which may be wide/narrow for other reasons) — ~35 degrees gives a
// comfortable margin around the selection instead of a tight crop.
constexpr float kFocusFovRadians = 35.0f * 3.14159265358979323846f / 180.0f;
constexpr float kDefaultHalfExtent = 0.5f;  // 1m default cube for part-less entities

} // namespace

void focus_camera_on_selection(matter::CameraDesc& camera,
                               const SelectionSet& selection,
                               const FieldCommands& fields) {
    if (selection.empty() || !fields.get_float3) return;

    Aabb box{};
    for (const SelectedObject& obj : selection.items()) {
        if (obj.kind != SelectedObject::Entity) continue;
        const matter::scene::SceneEntityId id{obj.id};
        matter::Float3 translation{};
        if (!fields.get_float3(id, "LocalTransform", "translation", translation))
            continue;
        expand(box, translation, kDefaultHalfExtent);
    }
    if (!box.valid) return;  // nothing focusable (empty/baked-root-only selection)

    const matter::Float3 center{
        (box.min.x + box.max.x) * 0.5f,
        (box.min.y + box.max.y) * 0.5f,
        (box.min.z + box.max.z) * 0.5f,
    };
    const matter::Float3 extent{
        box.max.x - box.min.x,
        box.max.y - box.min.y,
        box.max.z - box.min.z,
    };
    float radius = 0.5f * std::sqrt(extent.x * extent.x + extent.y * extent.y +
                                    extent.z * extent.z);
    radius = std::max(radius, kDefaultHalfExtent);

    // Preserve the current view direction (position - target); only the
    // distance changes.
    matter::Float3 dir{camera.position.x - camera.target.x,
                       camera.position.y - camera.target.y,
                       camera.position.z - camera.target.z};
    float dir_len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dir_len < 1e-6f) {
        dir = {0.0f, 0.0f, 1.0f};
        dir_len = 1.0f;
    }
    dir.x /= dir_len;
    dir.y /= dir_len;
    dir.z /= dir_len;

    const float distance =
        std::max(radius / std::tan(kFocusFovRadians * 0.5f), 0.1f);

    camera.target = center;
    camera.position = {center.x + dir.x * distance, center.y + dir.y * distance,
                       center.z + dir.z * distance};
}

} // namespace viewer
