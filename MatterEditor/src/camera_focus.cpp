// MatterEditor/src/camera_focus.cpp
//
// Implementation of the selection focus/framing routines declared in
// camera_focus.h.
//
// The whole file is one idea: merge every selected item into a single
// world-space AABB, then derive from it (a) a pivot point plus a bounding
// sphere radius, and (b) a camera distance that fits that sphere in a fixed
// framing FOV. Both consumers -- the F-key focus and the "Orbit selection"
// pivot -- read the SAME merge, which is the point of splitting
// selection_focus_point out: they can never disagree about where the selection
// is.
//
// What contributes to the merge, per selected item:
//   - BakedRoot: the eight corners of its OBB, transformed to world space and
//     re-fit axis-aligned. Requires the caller's bounds callback; skipped
//     entirely when it is absent or reports failure.
//   - anything else: a fixed 1 m cube around LocalTransform.translation, read
//     through FieldCommands. Part-aware entity bounds are still future work.
//
// An item that resolves to nothing is skipped, not defaulted -- and if NOTHING
// resolves, both functions report failure and the camera is left exactly as it
// was. A selection that silently framed the origin would be worse than one that
// does nothing.
//
// Units are world units (metres at editor scale) throughout; matrices are
// row-major (see transform_point).

#include "camera_focus.h"

#include <algorithm>
#include <cmath>

namespace viewer {
namespace {

// Accumulating world-space AABB. `valid` is false until the first expand, which
// is what distinguishes "empty" from "a degenerate box at the origin" -- the
// distinction both public functions key their failure result off.
struct Aabb {
    matter::Float3 min{};
    matter::Float3 max{};
    bool valid = false;
};

// Union `box` with the span [lo, hi]. The first call seeds the box outright
// rather than growing from a zero box, so the origin is never included by
// accident. Callers pass lo == hi to add a single point.
void expand_span(Aabb& box, const matter::Float3& lo, const matter::Float3& hi) {
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

void expand(Aabb& box, const matter::Float3& center, float half_extent) {
    expand_span(box,
                {center.x - half_extent, center.y - half_extent, center.z - half_extent},
                {center.x + half_extent, center.y + half_extent, center.z + half_extent});
}

// Row-major 4x4 with translation in m[3]/m[7]/m[11] — the same layout
// selection_bounds.cpp writes into SelectionBounds::world_matrix.
matter::Float3 transform_point(const float m[16], float x, float y, float z) {
    return {m[0] * x + m[1] * y + m[2] * z + m[3],
            m[4] * x + m[5] * y + m[6] * z + m[7],
            m[8] * x + m[9] * y + m[10] * z + m[11]};
}

// Merge a BakedRoot's OBB into the box by expanding with each transformed
// corner — an axis-aligned re-fit of the world-space box, matching what the
// selection outline draws.
void expand_baked(Aabb& box, const SelectionBounds& sb) {
    for (int i = 0; i < 8; ++i) {
        const float x = (i & 1) ? sb.local_max[0] : sb.local_min[0];
        const float y = (i & 2) ? sb.local_max[1] : sb.local_min[1];
        const float z = (i & 4) ? sb.local_max[2] : sb.local_min[2];
        const matter::Float3 p = transform_point(sb.world_matrix, x, y, z);
        expand_span(box, p, p);
    }
}

// Fixed framing FOV independent of the live camera's own vertical_fov_radians
// (which may be wide/narrow for other reasons) — ~35 degrees gives a
// comfortable margin around the selection instead of a tight crop.
constexpr float kFocusFovRadians = 35.0f * 3.14159265358979323846f / 180.0f;
constexpr float kDefaultHalfExtent = 0.5f;  // 1m default cube for part-less entities

} // namespace

// Merges the selection into one AABB and reports its centre and bounding-sphere
// radius. Returns false -- leaving both out-parameters untouched -- for an empty
// selection and for a selection where nothing resolved to bounds; those are the
// same two cases in which focus_camera_on_selection leaves the camera alone.
//
// Cost is O(selection size), with one FieldCommands lookup or one bounds
// callback per item; there is no caching, so a caller polling this every frame
// (the orbit pivot does) pays it every frame.
bool selection_focus_point(const SelectionSet& selection,
                           const FieldCommands& fields,
                           const BakedRootBoundsFn& baked_bounds,
                           matter::Float3& out_center, float& out_radius) {
    if (selection.empty()) return false;

    Aabb box{};
    for (const SelectedObject& obj : selection.items()) {
        if (obj.kind == SelectedObject::BakedRoot) {
            SelectionBounds sb{};
            if (baked_bounds && baked_bounds(obj.id, sb)) expand_baked(box, sb);
            continue;
        }
        if (!fields.get_float3) continue;
        const matter::scene::SceneEntityId id{obj.id};
        matter::Float3 translation{};
        if (!fields.get_float3(id, "LocalTransform", "translation", translation))
            continue;
        expand(box, translation, kDefaultHalfExtent);
    }
    if (!box.valid) return false;  // nothing focusable resolved to bounds

    out_center = {
        (box.min.x + box.max.x) * 0.5f,
        (box.min.y + box.max.y) * 0.5f,
        (box.min.z + box.max.z) * 0.5f,
    };
    const matter::Float3 extent{
        box.max.x - box.min.x,
        box.max.y - box.min.y,
        box.max.z - box.min.z,
    };
    out_radius = 0.5f * std::sqrt(extent.x * extent.x + extent.y * extent.y +
                                  extent.z * extent.z);
    // Floor the radius so a single point-like item cannot produce a zero
    // framing distance and put the camera inside its own pivot.
    out_radius = std::max(out_radius, kDefaultHalfExtent);
    return true;
}

// Instant snap, no animation: the view DIRECTION is preserved and only the
// target and the distance change, so focusing does not disorient the user by
// also reorienting them. A degenerate current direction (position == target)
// falls back to +Z. The distance is floored at 0.1 units so a tiny selection
// cannot pull the camera into the geometry.
void focus_camera_on_selection(matter::CameraDesc& camera,
                               const SelectionSet& selection,
                               const FieldCommands& fields,
                               const BakedRootBoundsFn& baked_bounds) {
    matter::Float3 center{};
    float radius = 0.0f;
    if (!selection_focus_point(selection, fields, baked_bounds, center, radius))
        return;

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
