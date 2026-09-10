#pragma once

// MatterEditor/src/viewport_capture.h
//
// Everything about `viewport.capture` that can be wrong WITHOUT a renderer:
// the pending-capture state machine, the capture-time geometry an agent needs
// to read the PNG it just got back, the image<->viewport coordinate mapping
// that closes the loop into `viewport.pick`, and the selection annotations
// projected into that same image.
//
// It deliberately owns no Vulkan, GLFW, ImGui or WorldSession state.
// MatterEditor/src/main.cpp adapts the live sources into these plain structs
// at the one seam where a capture actually lands, which is what lets
// MatterEditor/tests/test_viewport_capture.cpp exercise timeout, abandoned
// presentation, a resize between arm and capture, and pick-from-capture
// coherence with no window at all.
//
// WHY A STATE MACHINE. A screenshot is the one agent command that cannot
// answer from the app lane: it is only true once a frame has PRESENTED and its
// readback has been written to disk. The registry handler therefore ARMS a
// capture and returns nothing terminal; the frame loop resolves it later
// through viewer::agent::Protocol::complete(). Exactly one terminal result is
// still emitted, because the protocol's own pending map is the gate.
//
// SPACES, and there are three:
//   - viewport-local LOGICAL pixels: what `viewport.pick` takes. (0,0) is the
//     top-left of the 3D viewport content.
//   - FRAMEBUFFER pixels: logical scaled by the display's framebuffer scale.
//   - IMAGE pixels: the decoded PNG. The capture is a framebuffer readback, so
//     image pixels are framebuffer pixels offset by the crop origin (zero for
//     an uncropped capture).
// Everything below is explicit about which one it means; nothing guesses.

#include "agent_protocol.h"
#include "matter/camera.h"
#include "matter/json_doc.h"
#include "selection_bounds.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace viewer::capture {

using Clock = std::chrono::steady_clock;

// A rectangle in one of the three spaces above. Floating point because the
// logical rectangle genuinely is: ImGui content regions land on fractional
// pixels at fractional DPI scales, and rounding here would put the annotation
// boxes and the pick mapping half a pixel apart.
struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// Measured on the frame that ACTUALLY presented and was read back -- never at
// arm time. A window resize between the two is therefore reported as the size
// the image really is, which is the whole reason this is a separate struct
// rather than a snapshot taken beside the request.
struct Geometry {
    // The 3D viewport content rectangle, in logical pixels, within the window.
    Rect viewport_logical;
    // Logical -> framebuffer pixel scale for this display/frame.
    double framebuffer_scale_x = 1.0;
    double framebuffer_scale_y = 1.0;
    // The written PNG's own dimensions.
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
    // Where the written image starts inside the framebuffer. A full-swapchain
    // capture leaves this at (0,0); a cropped one (issue replay) does not.
    double image_origin_x = 0.0;
    double image_origin_y = 0.0;
    // False while the Part Workbench isolation view owned the viewport. The
    // PNG is still a true picture of what was on screen, but the selection,
    // the picker and the production camera all describe the OTHER world, so
    // annotations are reported unavailable rather than projected into it.
    bool production_view = true;
};

// The viewport rectangle expressed in framebuffer and in image pixels.
Rect viewport_in_framebuffer(const Geometry& geometry);
Rect viewport_in_image(const Geometry& geometry);

// Image pixel -> viewport-local logical pixel: the exact coordinate
// `viewport.pick` takes. Returns false when the pixel is outside the
// viewport's part of the image (UI chrome, or a capture whose viewport is
// wholly outside the crop), leaving the outputs untouched -- a caller must not
// silently pick the nearest edge instead.
bool image_to_viewport_logical(const Geometry& geometry, double image_x,
                               double image_y, double& out_x, double& out_y);

// The inverse. Returns false for a coordinate outside the live viewport
// rectangle, matching the bound `viewport.pick` itself enforces.
bool viewport_logical_to_image(const Geometry& geometry, double logical_x,
                               double logical_y, double& out_image_x,
                               double& out_image_y);

// ---------------------------------------------------------------------------
// Pending-capture state machine
// ---------------------------------------------------------------------------

struct Request {
    std::string request_id;
    std::uint64_t ticket_id = 0;
    std::string path;            // the PNG the capture writes
    bool annotate = false;       // include projected selection annotations
    Clock::time_point deadline{};
};

// Why a pending capture ended. Each maps to exactly one protocol status; the
// mapping is `terminal_for` below rather than being spelled out at each of
// main.cpp's four resolution sites.
enum class Resolution {
    Captured,          // a presented frame was read back and the PNG written
    TimedOut,          // the request's own deadline passed first
    Abandoned,         // the editor stopped presenting; the shot deadman fired
    WriteFailed,       // readback or PNG write failed
};

struct Terminal {
    agent::Status status = agent::Status::Ok;
    std::string message;
};

Terminal terminal_for(Resolution resolution);

// At most ONE capture is in flight. A second `viewport.capture` while one is
// armed is refused rather than queued: two agents interleaving screenshots
// through one FIFO would otherwise each get the other's geometry, and a queue
// would make the request's own timeout meaningless.
class Tracker {
public:
    bool armed() const { return armed_; }
    const Request& request() const { return request_; }

    // False when a capture is already armed.
    bool arm(Request request);

    // Is `path` the capture this tracker is waiting for? The FIFO `shot_now`
    // verb shares the same present/readback queue, so a landed capture is only
    // ours when the paths match.
    bool owns(const std::string& path) const;

    bool expired(Clock::time_point now) const;

    // Clears the armed state and hands back the request. Calling it while not
    // armed returns a default-constructed Request.
    Request release();

private:
    bool armed_ = false;
    Request request_;
};

// ---------------------------------------------------------------------------
// Selection annotations
// ---------------------------------------------------------------------------

// One selected object handed to the projector. `resolved` false means
// selection_bounds could not place it this frame (a stale entry the next
// SelectionSet::validate will prune); it is reported as unavailable rather
// than dropped, so the annotation list and the selection list agree in length.
struct AnnotationInput {
    agent::ObjectIdentity object;
    bool primary = false;
    bool resolved = false;
    SelectionBounds bounds{};
};

struct Annotation {
    agent::ObjectIdentity object;
    bool primary = false;
    bool available = false;
    std::string reason;          // set only when !available
    Rect image_rect;             // axis-aligned box of the projected OBB
    Rect viewport_logical_rect;
    bool clipped = false;        // the box extends past the viewport edges
};

// Project each input's oriented box into the captured image. The result is the
// screen-aligned bounding rectangle of the eight projected corners, clamped to
// the viewport's part of the image; `clipped` says whether clamping removed
// anything. An object with a corner at or behind the eye is reported
// unavailable rather than given a wild rectangle.
//
// It never touches authored content and never touches the SelectionSet: an
// annotation is a measurement of the image, not an edit of the world.
std::vector<Annotation> project_annotations(
    const std::vector<AnnotationInput>& inputs,
    const matter::CameraDesc& camera, const Geometry& geometry);

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

// The `result` object of a successful viewport.capture. `annotations` is
// present either way: an explicit {"available":false,"reason":...} when the
// caller did not ask for annotations, so a reader never has to tell "absent"
// from "empty selection".
matter::jsondoc::Value capture_result_json(
    const Request& request, const Geometry& geometry,
    const matter::CameraDesc& camera, const agent::Context& context,
    const std::vector<Annotation>* annotations);

// Argument extraction for viewport.capture. The descriptor has already checked
// JSON types; this applies the defaults and the non-empty rule. The PNG PATH
// POLICY is deliberately NOT applied here -- main.cpp runs the same
// fifo_safe_absolute_png_path the `shot`/`shot_now` verbs use, so the two can
// never diverge on what a safe capture path is.
struct Arguments {
    std::string path;
    bool annotate = false;
};
bool parse_arguments(const matter::jsondoc::Value& arguments, Arguments& out,
                     std::string& error);

}  // namespace viewer::capture
