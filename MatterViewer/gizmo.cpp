#include "gizmo.h"

#include <cmath>

#include "imgui.h"
#include "ImGuizmo.h"

// ---------------------------------------------------------------------------
// Matrix convention notes
//
// ImGuizmo (like DirectX) stores 4x4 matrices row-major and treats vectors as
// ROWS, so a transform is applied as v' = v * M and matrices compose
// left-to-right in the order they are applied (Scale * Rotation *
// Translation for a typical object-to-world matrix). Concretely, for a
// matrix laid out as m16[row*4 + col]:
//   row 0 (m16[0..3])  = local +X axis, scaled
//   row 1 (m16[4..7])  = local +Y axis, scaled
//   row 2 (m16[8..11]) = local +Z axis, scaled
//   row 3 (m16[12..15]) = translation (m16[12..14]) with w = m16[15] = 1
// This file builds the view, projection, and object matrices directly in
// that layout rather than routing through matter::Mat4f (whose column-vector
// convention is different) or ImGuizmo's own Decompose/Recompose helpers
// (which round-trip rotation through Euler angles and would fight the
// quaternion storage FieldCommands uses for LocalTransform.rotation).
// ---------------------------------------------------------------------------

namespace viewer {
namespace {

// Right-handed look-at, matching ImGuizmo::LookAt(..., rightHanded=true).
void build_view_matrix(const matter::Float3& eye, const matter::Float3& at,
                       const matter::Float3& up, float* m16) {
    auto sub = [](matter::Float3 a, matter::Float3 b) {
        return matter::Float3{a.x - b.x, a.y - b.y, a.z - b.z};
    };
    auto normalize = [](matter::Float3 v) {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        const float inv = 1.0f / (len + 1e-8f);
        return matter::Float3{v.x * inv, v.y * inv, v.z * inv};
    };
    auto cross = [](matter::Float3 a, matter::Float3 b) {
        return matter::Float3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                              a.x * b.y - a.y * b.x};
    };
    auto dot = [](matter::Float3 a, matter::Float3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };

    // Right-handed: camera looks down -Z, so Z axis points from target to eye.
    const matter::Float3 z = normalize(sub(eye, at));
    const matter::Float3 y_hint = normalize(up);
    const matter::Float3 x = normalize(cross(y_hint, z));
    const matter::Float3 y = normalize(cross(z, x));

    m16[0] = x.x;  m16[1] = y.x;  m16[2] = z.x;   m16[3] = 0.0f;
    m16[4] = x.y;  m16[5] = y.y;  m16[6] = z.y;   m16[7] = 0.0f;
    m16[8] = x.z;  m16[9] = y.z;  m16[10] = z.z;  m16[11] = 0.0f;
    m16[12] = -dot(x, eye);
    m16[13] = -dot(y, eye);
    m16[14] = -dot(z, eye);
    m16[15] = 1.0f;
}

// Right-handed perspective projection, matching
// ImGuizmo::Perspective(..., rightHanded=true).
void build_projection_matrix(float vertical_fov_radians, float aspect,
                             float near_plane, float far_plane, float* m16) {
    const float ymax = near_plane * std::tan(vertical_fov_radians * 0.5f);
    const float xmax = ymax * aspect;
    const float left = -xmax, right = xmax, bottom = -ymax, top = ymax;
    const float temp = 2.0f * near_plane;
    const float temp2 = right - left;
    const float temp3 = top - bottom;
    const float temp4 = far_plane - near_plane;

    m16[0] = temp / temp2;  m16[1] = 0.0f;           m16[2] = 0.0f;                                  m16[3] = 0.0f;
    m16[4] = 0.0f;          m16[5] = temp / temp3;   m16[6] = 0.0f;                                  m16[7] = 0.0f;
    m16[8] = (right + left) / temp2;
    m16[9] = (top + bottom) / temp3;
    m16[10] = -(far_plane + near_plane) / temp4;
    m16[11] = -1.0f;
    m16[12] = 0.0f;
    m16[13] = 0.0f;
    m16[14] = (-temp * far_plane) / temp4;
    m16[15] = 0.0f;
}

// Builds an object-to-world matrix in ImGuizmo's row-vector layout (see file
// header) from a translation + quaternion rotation + non-uniform scale.
void compose_matrix(const matter::Float3& translation,
                    const matter::Quaternion& rotation,
                    const matter::Float3& scale, float* m16) {
    const float x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;

    // Columns of the standard column-vector rotation matrix R (v' = R*v)
    // become the rows of this row-vector-convention matrix, since each row
    // here is R applied to a local basis axis: row0 = R*(1,0,0) = column 0.
    const float r00 = 1.0f - 2.0f * (y * y + z * z);
    const float r10 = 2.0f * (x * y + w * z);
    const float r20 = 2.0f * (x * z - w * y);

    const float r01 = 2.0f * (x * y - w * z);
    const float r11 = 1.0f - 2.0f * (x * x + z * z);
    const float r21 = 2.0f * (y * z + w * x);

    const float r02 = 2.0f * (x * z + w * y);
    const float r12 = 2.0f * (y * z - w * x);
    const float r22 = 1.0f - 2.0f * (x * x + y * y);

    m16[0] = r00 * scale.x;  m16[1] = r10 * scale.x;  m16[2] = r20 * scale.x;  m16[3] = 0.0f;
    m16[4] = r01 * scale.y;  m16[5] = r11 * scale.y;  m16[6] = r21 * scale.y;  m16[7] = 0.0f;
    m16[8] = r02 * scale.z;  m16[9] = r12 * scale.z;  m16[10] = r22 * scale.z; m16[11] = 0.0f;
    m16[12] = translation.x; m16[13] = translation.y; m16[14] = translation.z; m16[15] = 1.0f;
}

// Inverse of compose_matrix: extracts translation/rotation/scale straight
// from the row-vector matrix, avoiding ImGuizmo's Euler-angle round trip so
// the result maps cleanly back onto FieldCommands' quaternion storage.
void decompose_matrix(const float* m16, matter::Float3& translation,
                      matter::Quaternion& rotation, matter::Float3& scale) {
    auto row_len = [&](int r) {
        const float a = m16[r * 4 + 0], b = m16[r * 4 + 1], c = m16[r * 4 + 2];
        return std::sqrt(a * a + b * b + c * c);
    };
    scale.x = row_len(0);
    scale.y = row_len(1);
    scale.z = row_len(2);
    // TODO: detect determinant < 0 and preserve one axis's negative sign
    // for mirrored/negative-scale objects.

    const float sx = scale.x > 1e-8f ? 1.0f / scale.x : 0.0f;
    const float sy = scale.y > 1e-8f ? 1.0f / scale.y : 0.0f;
    const float sz = scale.z > 1e-8f ? 1.0f / scale.z : 0.0f;

    // Normalized rows are the rotation matrix columns (see compose_matrix).
    const float r00 = m16[0] * sx, r10 = m16[1] * sx, r20 = m16[2] * sx;
    const float r01 = m16[4] * sy, r11 = m16[5] * sy, r21 = m16[6] * sy;
    const float r02 = m16[8] * sz, r12 = m16[9] * sz, r22 = m16[10] * sz;

    // Standard rotation-matrix -> quaternion (Shepperd's method).
    const float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        rotation.w = 0.25f * s;
        rotation.x = (r21 - r12) / s;
        rotation.y = (r02 - r20) / s;
        rotation.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        rotation.w = (r21 - r12) / s;
        rotation.x = 0.25f * s;
        rotation.y = (r01 + r10) / s;
        rotation.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        rotation.w = (r02 - r20) / s;
        rotation.x = (r01 + r10) / s;
        rotation.y = 0.25f * s;
        rotation.z = (r12 + r21) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        rotation.w = (r10 - r01) / s;
        rotation.x = (r02 + r20) / s;
        rotation.y = (r12 + r21) / s;
        rotation.z = 0.25f * s;
    }

    translation.x = m16[12];
    translation.y = m16[13];
    translation.z = m16[14];
}

ImGuizmo::OPERATION to_imguizmo_operation(GizmoOperation op) {
    switch (op) {
        case GizmoOperation::Translate: return ImGuizmo::TRANSLATE;
        case GizmoOperation::Rotate:    return ImGuizmo::ROTATE;
        case GizmoOperation::Scale:     return ImGuizmo::SCALE;
    }
    return ImGuizmo::TRANSLATE;
}

} // namespace

bool draw_gizmo(GizmoState& state, const SelectionSet& selection,
                const FieldCommands& fields, const matter::CameraDesc& camera,
                matter::scene::SimulationMode mode, float viewport_x,
                float viewport_y, float viewport_w, float viewport_h) {
    if (mode == matter::scene::SimulationMode::Play) return false;
    if (viewport_w <= 0.0f || viewport_h <= 0.0f) return false;

    const SelectedObject* primary = selection.primary();
    if (!primary || primary->kind != SelectedObject::Entity) return false;

    const matter::scene::SceneEntityId primary_id{primary->id};
    matter::Float3 translation{};
    matter::Quaternion rotation{};
    matter::Float3 scale{1.0f, 1.0f, 1.0f};
    const bool have_t = fields.get_float3 &&
                        fields.get_float3(primary_id, "LocalTransform",
                                          "translation", translation);
    const bool have_r = fields.get_quat &&
                        fields.get_quat(primary_id, "LocalTransform",
                                        "rotation", rotation);
    const bool have_s = fields.get_float3 &&
                        fields.get_float3(primary_id, "LocalTransform",
                                          "scale", scale);
    if (!have_t || !have_r) return false;
    if (!have_s) scale = {1.0f, 1.0f, 1.0f};

    float view[16];
    float projection[16];
    build_view_matrix(camera.position, camera.target, camera.up, view);
    build_projection_matrix(camera.vertical_fov_radians,
                            viewport_w / viewport_h, camera.near_plane,
                            camera.far_plane, projection);

    float object_matrix[16];
    compose_matrix(translation, rotation, scale, object_matrix);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(viewport_x, viewport_y, viewport_w, viewport_h);

    const bool manipulated = ImGuizmo::Manipulate(
        view, projection, to_imguizmo_operation(state.operation),
        ImGuizmo::LOCAL, object_matrix);

    if (manipulated) {
        matter::Float3 new_translation{};
        matter::Quaternion new_rotation{};
        matter::Float3 new_scale{};
        decompose_matrix(object_matrix, new_translation, new_rotation,
                         new_scale);

        if (fields.set_float3)
            fields.set_float3(primary_id, "LocalTransform", "translation",
                              new_translation);
        if (fields.set_quat)
            fields.set_quat(primary_id, "LocalTransform", "rotation",
                            new_rotation);
        if (fields.set_float3)
            fields.set_float3(primary_id, "LocalTransform", "scale",
                              new_scale);

        // Fan the primary's world-space translation delta out to the rest of
        // the selection. Rotate/scale edits only affect the primary entity.
        if (state.operation == GizmoOperation::Translate &&
            selection.items().size() > 1) {
            const matter::Float3 delta{
                new_translation.x - translation.x,
                new_translation.y - translation.y,
                new_translation.z - translation.z};
            if ((delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f) &&
                fields.get_float3 && fields.set_float3) {
                for (const SelectedObject& obj : selection.items()) {
                    if (obj == *primary) continue;
                    if (obj.kind != SelectedObject::Entity) continue;
                    const matter::scene::SceneEntityId other_id{obj.id};
                    matter::Float3 other_translation{};
                    if (!fields.get_float3(other_id, "LocalTransform",
                                           "translation", other_translation))
                        continue;
                    other_translation.x += delta.x;
                    other_translation.y += delta.y;
                    other_translation.z += delta.z;
                    fields.set_float3(other_id, "LocalTransform",
                                      "translation", other_translation);
                }
            }
        }
    }

    return true;
}

void update_gizmo_hotkeys(GizmoState& state) {
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) ||
        ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        state.operation = GizmoOperation::Translate;
    } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        state.operation = GizmoOperation::Rotate;
    } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        state.operation = GizmoOperation::Scale;
    }
}

} // namespace viewer
