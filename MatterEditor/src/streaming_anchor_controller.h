#pragma once

// MatterEditor/src/streaming_anchor_controller.h
//
// The editor's STREAMING ANCHOR: a Flecs entity carrying a LocalTransform and
// (once attached) matter::streaming::SectorStreaming, whose world position is
// what the sector streamer treats as "the viewer". By default the anchor
// follows the editor camera, so flying around pages sectors in and out; detach
// the follow and the anchor stays put, freezing which sectors are resident
// while you fly in to inspect them.
//
// Shape of this file: free functions over a `StreamingAnchorState` plus a
// `flecs::world&` handed in on every call. The state NEVER retains a world
// pointer — it keeps a `world_identity` token instead — so a world reload
// leaves no dangling reference; `validate_anchor` notices the identity changed
// and drops the selection. Every mutator below calls `validate_anchor` first,
// so callers do not have to.
//
// How it fits:
//   - `Ui` (ui.h `streaming_anchor_`) owns the one live StreamingAnchorState
//     and drives it from `ui.cpp` (validate / create / attach / follow_camera).
//   - `main.cpp` constructs a CurrentFrameInputOrder per frame and steps it
//     through the frame; the free-fly camera controller only runs when
//     `camera_update_allowed()` says the frame completed in order.
//   - The Properties panel's streaming editor (specialized_editors.h) is a
//     separate, entity-selection-driven path; do not confuse the two.
//
// Conventions: positions are world-space metres. `matter::Mat4f` is ROW-MAJOR
// storage with column-vector algebra, so translation is m[3]/m[7]/m[11];
// ImGuizmo wants column-major, which is what the to_/from_imguizmo_matrix pair
// is for.
//
// Threading: main/UI thread only. Flecs worlds are not touched concurrently
// here and nothing takes a lock.
//
// Deliberately free of ImGui, Vulkan and GLFW so it can be unit-tested
// headlessly: `MatterEngine3/tests/viewer_logic_tests.cpp`
// (`make -C MatterEngine3/tests run-viewer-logic`).

#include <array>
#include <cstdint>

#include "flecs.h"
#include "matter/camera.h"
#include "matter/ecs.h"

namespace matter_viewer {

// Which anchor the editor is driving, and how. Plain value type; safe to copy,
// though the editor keeps exactly one.
//
// `selected == 0` is the "no anchor" sentinel (0 is never a live Flecs id).
// `follow_editor_camera` does DOUBLE DUTY: it makes follow_camera() write the
// camera position into the anchor, and it also locks the gizmo — while it is
// set, gizmo_translation_allowed() returns false, because an anchor that is
// being dragged by the camera must not also be draggable by hand.
struct StreamingAnchorState {
    flecs::entity_t selected = 0;
    bool follow_editor_camera = true;

    // Unique private-world token; this controller never retains a Flecs world pointer.
    std::uint64_t world_identity = 0;
};

// A one-frame monotonic latch over the editor's frame phases. It exists to
// answer a single question at the end of the frame: may the free-fly camera
// controller consume this frame's input?
//
// Intended order, once each, in this sequence:
//   begin_ui() -> build_ui() -> decide_capture(allowed) -> tick_scene()
//   -> render_scene() -> end_frame()
// and only then does camera_update_allowed() return the value that was latched
// by decide_capture(). `allowed` is normally the result of the free function
// camera_input_allowed() below, i.e. "ImGui and the gizmo don't want the input".
//
// FAILS SILENTLY BY DESIGN: every transition is a no-op unless the previous
// stage was actually reached — no assert, no log. A skipped or out-of-order
// stage therefore wedges the latch where it is and camera_update_allowed()
// stays false for the rest of the frame, which loses camera motion rather than
// acting on a half-built frame. `main.cpp` constructs a fresh instance every
// frame, so the wedge never persists past the frame that caused it.
class CurrentFrameInputOrder {
public:
    void begin_ui() noexcept;
    void build_ui() noexcept;
    void decide_capture(bool camera_input_allowed) noexcept;
    void tick_scene() noexcept;
    void render_scene() noexcept;
    void end_frame() noexcept;
    bool camera_update_allowed() const noexcept;

private:
    enum class Stage : std::uint8_t {
        AwaitingUi,
        UiBegun,
        UiBuilt,
        CaptureDecided,
        SceneTicked,
        SceneRendered,
        FrameEnded
    };
    Stage stage_ = Stage::AwaitingUi;
    bool camera_input_allowed_ = false;
};

// Creates a NEW entity with a default LocalTransform, selects it, and turns
// follow-camera ON. It does not add SectorStreaming — attach_streaming does
// that. Returns the new entity id.
flecs::entity_t create_anchor(StreamingAnchorState& state, flecs::world& world);
// Selects an existing entity. Note two asymmetries with create_anchor:
// it CLEARS the current selection first, so a rejected candidate (id 0, dead
// entity, or no LocalTransform -> returns false) leaves NO selection rather
// than the previous one; and clearing also turns follow-camera OFF, so a
// hand-selected anchor does not start tracking the camera.
bool select_anchor(StreamingAnchorState& state, flecs::world& world,
                   flecs::entity_t anchor);
void clear_anchor(StreamingAnchorState& state);
// Drops the selection when the anchor died or when `world` is a different
// world than the one the selection was made in (detected through the identity
// token, since no world pointer is kept). Otherwise re-stamps the token — so a
// state with identity 0 is adopted by the world it is first validated against.
// Every mutator below calls this first; call it yourself once a frame if you
// only read the state.
void validate_anchor(StreamingAnchorState& state, flecs::world& world);
// Add / remove the SectorStreaming tag on the selected anchor. attach is
// idempotent and returns true when the tag was already there; remove returns
// false when there was nothing to remove. Both return false with no selection
// or when the anchor has no LocalTransform.
bool attach_streaming(StreamingAnchorState& state, flecs::world& world);
bool remove_streaming(StreamingAnchorState& state, flecs::world& world);
// Writes `camera_position` (world-space metres, 3 floats; null is tolerated
// and does nothing) into the anchor's LocalTransform translation and marks it
// TransformDirty. No-op unless follow_editor_camera is set. Rotation and scale
// are left alone.
void follow_camera(StreamingAnchorState& state, flecs::world& world,
                   const float camera_position[3]);
void detach_follow(StreamingAnchorState& state, flecs::world& world);
// Takes ONLY the translation out of a row-major 4x4 (m[3], m[7], m[11]) and
// writes it to the anchor, marking it TransformDirty. Any rotation or scale
// the gizmo produced is discarded — the anchor is a point, so there is nothing
// for them to mean. Returns false with no selection or a null matrix.
bool apply_gizmo_translation(StreamingAnchorState& state, flecs::world& world,
                             const float matrix[16]);
bool gizmo_translation_allowed(StreamingAnchorState& state,
                               flecs::world& world);
// Transform plumbing between the ECS and ImGuizmo.
//
// local_transform_matrix composes T*R*S into a ROW-MAJOR Mat4f, normalizing
// the rotation quaternion first; a non-finite or zero-length quaternion falls
// back to identity rotation rather than producing NaNs.
//
// to_/from_imguizmo_matrix are pure transposes: ImGuizmo works in column-major
// float[16], Mat4f is row-major. from_imguizmo_matrix returns a zeroed matrix
// for a null pointer.
matter::Mat4f local_transform_matrix(const matter::ecs::LocalTransform& transform);
std::array<float, 16> to_imguizmo_matrix(const matter::Mat4f& matrix);
matter::Mat4f from_imguizmo_matrix(const float matrix[16]);
// "Frame the anchor": mutates `camera` in place so it looks at the selected
// anchor from `distance` metres away along its CURRENT view direction — the
// heading is preserved, only the pivot and the range change. `distance` must
// be finite and > 0. A degenerate camera (position == target) falls back to
// looking down -Z. Returns false, leaving the camera untouched, when there is
// no selection or the anchor has no LocalTransform.
bool frame_selected_anchor(StreamingAnchorState& state, flecs::world& world,
                           matter::CameraDesc& camera, float distance);
// Pure predicate, no state: true only when none of ImGui's two capture flags
// and neither gizmo flag is set. This is what gets latched into
// CurrentFrameInputOrder::decide_capture. Kept as a free function so the truth
// table is testable without an ImGui context.
bool camera_input_allowed(bool want_capture_mouse, bool want_capture_keyboard,
                          bool gizmo_over, bool gizmo_using);

} // namespace matter_viewer
