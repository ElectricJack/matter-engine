// MatterEditor/src/gizmo.cpp
//
// The viewport's translate/rotate/scale handle, backed by ImGuizmo. Reads the
// primary selection's LocalTransform and its parent's world matrix through the
// FieldCommands getters, composes them into the WORLD matrix ImGuizmo
// manipulates, and writes the result back — divided by the parent again, so
// what lands in the ECS is local — through the same closure set. See gizmo.h
// for the public contract and the matrix-convention block below for why this
// file builds its own matrices.
//
// ImGui/main thread only, inside the ImGui frame and after
// ImGuizmo::BeginFrame().

#include "gizmo.h"

#include <cmath>
#include <cstring>

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
//
// The ONE matter::Mat4f that reaches this file is the parent world matrix
// from FieldCommands::get_parent_world_matrix, and to_imguizmo_layout()
// transposes it on the way in — the two conventions are exact transposes of
// each other, which is also why the composition order flips (see there).
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

    // Row lengths are non-negative, so on their own they cannot express a
    // MIRRORED matrix — one whose upper 3x3 has a negative determinant. Left
    // unhandled, the normalized rows below would be a left-handed basis and
    // the quaternion extraction would silently produce a rotation that does
    // not reproduce the input. Fold the flip into ONE axis (X, by convention:
    // which axis carries the sign is not recoverable from the matrix, and any
    // single-axis choice recomposes to exactly the same matrix).
    const float determinant =
        m16[0] * (m16[5] * m16[10] - m16[6] * m16[9]) -
        m16[1] * (m16[4] * m16[10] - m16[6] * m16[8]) +
        m16[2] * (m16[4] * m16[9]  - m16[5] * m16[8]);
    if (determinant < 0.0f) scale.x = -scale.x;

    const float sx = std::fabs(scale.x) > 1e-8f ? 1.0f / scale.x : 0.0f;
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

// matter::Mat4f (row-major storage, COLUMN-vector algebra: translation in
// m[3], m[7], m[11] — the layout matter::ecs::WorldTransform uses) into
// ImGuizmo's row-vector layout (translation in m16[12..14]). The two are
// transposes of each other, and composition order flips with them: a
// column-vector `parent * local` is a row-vector `local * parent`.
void to_imguizmo_layout(const matter::Mat4f& in, float* m16) {
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            m16[row * 4 + column] = in.m[column * 4 + row];
}

// out = a * b, both in ImGuizmo's row-vector layout, so `a` is applied first.
// `out` must not alias either input.
void multiply_m16(const float* a, const float* b, float* out) {
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int inner = 0; inner < 4; ++inner)
                sum += a[row * 4 + inner] * b[inner * 4 + column];
            out[row * 4 + column] = sum;
        }
}

// Inverse of an AFFINE row-vector matrix — the bottom column (m[3], m[7],
// m[11], m[15]) is assumed to be (0, 0, 0, 1), which every parent world matrix
// the transform systems produce satisfies. Returns false and leaves `out`
// untouched when the upper 3x3 is singular (a parent collapsed to zero scale),
// which is the caller's signal to fall back to the parentless path rather than
// divide by zero.
bool invert_affine_m16(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[4], e = m[5], f = m[6];
    const float g = m[8], h = m[9], i = m[10];

    const float cof00 =  (e * i - f * h);
    const float cof10 = -(d * i - f * g);
    const float cof20 =  (d * h - e * g);
    const float determinant = a * cof00 + b * cof10 + c * cof20;
    if (!(std::fabs(determinant) > 1e-12f)) return false;
    const float inv_det = 1.0f / determinant;

    // Adjugate / determinant, in the same row-vector indexing as the input.
    const float n00 =  cof00 * inv_det;
    const float n01 = -(b * i - c * h) * inv_det;
    const float n02 =  (b * f - c * e) * inv_det;
    const float n10 =  cof10 * inv_det;
    const float n11 =  (a * i - c * g) * inv_det;
    const float n12 = -(a * f - c * d) * inv_det;
    const float n20 =  cof20 * inv_det;
    const float n21 = -(a * h - b * g) * inv_det;
    const float n22 =  (a * e - b * d) * inv_det;

    const float tx = m[12], ty = m[13], tz = m[14];
    out[0] = n00;  out[1] = n01;  out[2] = n02;  out[3] = 0.0f;
    out[4] = n10;  out[5] = n11;  out[6] = n12;  out[7] = 0.0f;
    out[8] = n20;  out[9] = n21;  out[10] = n22; out[11] = 0.0f;
    // Row-vector translation: the inverse offset is -t applied through the
    // inverted basis, i.e. -(t * N).
    out[12] = -(tx * n00 + ty * n10 + tz * n20);
    out[13] = -(tx * n01 + ty * n11 + tz * n21);
    out[14] = -(tx * n02 + ty * n12 + tz * n22);
    out[15] = 1.0f;
    return true;
}

// Rotate/scale a DIRECTION (w = 0) by a row-vector matrix, ignoring its
// translation row. Used to carry a world-space translation delta into another
// entity's parent space.
matter::Float3 transform_direction(const float* m16, const matter::Float3& v) {
    return matter::Float3{
        v.x * m16[0] + v.y * m16[4] + v.z * m16[8],
        v.x * m16[1] + v.y * m16[5] + v.z * m16[9],
        v.x * m16[2] + v.y * m16[6] + v.z * m16[10]};
}

// The parent's local->world matrix in ImGuizmo layout, plus its inverse.
// `valid` false means "treat the entity as parentless" — either no accessor is
// wired, the entity did not resolve, or the parent matrix is not invertible.
struct ParentFrame {
    float matrix[16] = {};
    float inverse[16] = {};
    bool valid = false;
};

ParentFrame parent_frame_for(const FieldCommands& fields,
                             matter::scene::SceneEntityId id) {
    ParentFrame frame;
    if (!fields.get_parent_world_matrix) return frame;
    matter::Mat4f parent_world{};
    if (!fields.get_parent_world_matrix(id, parent_world)) return frame;
    to_imguizmo_layout(parent_world, frame.matrix);
    frame.valid = invert_affine_m16(frame.matrix, frame.inverse);
    return frame;
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

// One frame of the gizmo. Values are re-read from FieldCommands every frame,
// so an edit made in the Properties panel moves the handle immediately.
//
// Three things to know before touching this:
//
//  - The matrix handed to ImGuizmo is a WORLD matrix: LocalTransform composed
//    with the parent's accumulated WorldTransform, fetched through
//    FieldCommands::get_parent_world_matrix. The manipulated result is carried
//    back through the parent's inverse before it is decomposed, so what gets
//    written is always a LOCAL transform. When that accessor is unset or the
//    parent matrix is singular, the entity is treated as parentless and this
//    degrades to composing LocalTransform alone.
//  - Only the channel the active operation edits is written back. A translate
//    drag writes translation and leaves the stored rotation quaternion and
//    scale untouched, so a drag cannot perturb values it did not move.
//    (decompose_matrix still cannot recover a negative (mirrored) scale — see
//    its TODO — but a translate/rotate drag no longer round-trips through it.)
//  - The multi-select fan-out is a WORLD-space translation delta, converted
//    into each other entity's own parent space before it is added to that
//    entity's LocalTransform.translation.
//
// Returns true whenever a gizmo was DRAWN, not whenever it was used; the
// caller needs that to suppress camera input while the handle is hovered.
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

    float local_matrix[16];
    compose_matrix(translation, rotation, scale, local_matrix);

    const ParentFrame parent = parent_frame_for(fields, primary_id);
    float object_matrix[16];
    if (parent.valid)
        multiply_m16(local_matrix, parent.matrix, object_matrix);
    else
        std::memcpy(object_matrix, local_matrix, sizeof(object_matrix));

    // World-space origin before the drag, for the multi-select fan-out below.
    const matter::Float3 world_before{object_matrix[12], object_matrix[13],
                                      object_matrix[14]};

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(viewport_x, viewport_y, viewport_w, viewport_h);

    const bool manipulated = ImGuizmo::Manipulate(
        view, projection, to_imguizmo_operation(state.operation),
        ImGuizmo::LOCAL, object_matrix);

    if (manipulated) {
        const matter::Float3 world_after{object_matrix[12], object_matrix[13],
                                         object_matrix[14]};

        // Back out of the parent's frame: what LocalTransform stores is always
        // relative to the parent, so the manipulated world matrix has to be
        // divided by it before it can be decomposed.
        float new_local[16];
        if (parent.valid)
            multiply_m16(object_matrix, parent.inverse, new_local);
        else
            std::memcpy(new_local, object_matrix, sizeof(new_local));

        matter::Float3 new_translation{};
        matter::Quaternion new_rotation{};
        matter::Float3 new_scale{};
        decompose_matrix(new_local, new_translation, new_rotation, new_scale);

        // Write back ONLY the channel this operation edits. ImGuizmo's LOCAL
        // handles are single-channel by construction — translate moves the
        // translation row, rotate turns the basis about the object's own
        // origin, scale rescales the basis rows — so writing the other two
        // fields would only feed back decompose_matrix's round-tripping error
        // (and, for a non-uniformly scaled parent, a shear the TRS triple
        // cannot represent anyway).
        switch (state.operation) {
            case GizmoOperation::Translate:
                if (fields.set_float3)
                    fields.set_float3(primary_id, "LocalTransform",
                                      "translation", new_translation);
                break;
            case GizmoOperation::Rotate:
                if (fields.set_quat)
                    fields.set_quat(primary_id, "LocalTransform", "rotation",
                                    new_rotation);
                break;
            case GizmoOperation::Scale:
                if (fields.set_float3)
                    fields.set_float3(primary_id, "LocalTransform", "scale",
                                      new_scale);
                break;
        }

        // Fan the primary's WORLD-space translation delta out to the rest of
        // the selection, converting it into each target's own parent space so
        // every selected entity moves the same distance in the world rather
        // than the same distance in its own parent's (possibly rotated or
        // scaled) frame. Rotate/scale edits only affect the primary entity.
        if (state.operation == GizmoOperation::Translate &&
            selection.items().size() > 1) {
            const matter::Float3 world_delta{world_after.x - world_before.x,
                                             world_after.y - world_before.y,
                                             world_after.z - world_before.z};
            if ((world_delta.x != 0.0f || world_delta.y != 0.0f ||
                 world_delta.z != 0.0f) &&
                fields.get_float3 && fields.set_float3) {
                for (const SelectedObject& obj : selection.items()) {
                    if (obj == *primary) continue;
                    if (obj.kind != SelectedObject::Entity) continue;
                    const matter::scene::SceneEntityId other_id{obj.id};
                    matter::Float3 other_translation{};
                    if (!fields.get_float3(other_id, "LocalTransform",
                                           "translation", other_translation))
                        continue;
                    const ParentFrame other_parent =
                        parent_frame_for(fields, other_id);
                    const matter::Float3 local_delta =
                        other_parent.valid
                            ? transform_direction(other_parent.inverse,
                                                  world_delta)
                            : world_delta;
                    other_translation.x += local_delta.x;
                    other_translation.y += local_delta.y;
                    other_translation.z += local_delta.z;
                    fields.set_float3(other_id, "LocalTransform",
                                      "translation", other_translation);
                }
            }
        }
    }

    return true;
}

// Edge-triggered (IsKeyPressed with repeat = false) and first-match-wins, so
// holding a key does not thrash the mode. The caller is responsible for not
// calling this while ImGui wants the keyboard — see gizmo.h.
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
