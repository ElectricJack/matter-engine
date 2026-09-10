// Headless cover for viewport.capture's rules: the pending-capture state
// machine, the geometry recorded on the frame that actually presented, the
// image<->viewport mapping that closes the loop into viewport.pick, and the
// selection annotations projected into the captured image.
//
// Nothing here opens a window. That is the point: the four failures the
// acceptance criteria name -- a capture that times out, a presentation that
// never lands, a resize between arming and capturing, and a pixel that maps
// back to the wrong pick coordinate -- are all decidable from plain values.

#include "../src/viewport_capture.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using matter::jsondoc::Value;
namespace cap = viewer::capture;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__,      \
                         __LINE__);                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

const Value& field(const Value& parent, const char* key, const char* message) {
    const Value* found = parent.find(key);
    CHECK(found != nullptr, message);
    return *found;
}

// A 1600x900 framebuffer at 2x scale whose 3D viewport is inset by the docked
// panels: logical rect (250, 40) 550x410, i.e. image rect (500, 80) 1100x820.
cap::Geometry docked_geometry() {
    cap::Geometry geometry;
    geometry.viewport_logical = cap::Rect{250.0, 40.0, 550.0, 410.0};
    geometry.framebuffer_scale_x = 2.0;
    geometry.framebuffer_scale_y = 2.0;
    geometry.image_width = 1600;
    geometry.image_height = 900;
    return geometry;
}

cap::Request request(const char* path, bool annotate,
                     cap::Clock::duration timeout) {
    cap::Request pending;
    pending.request_id = "req-1";
    pending.ticket_id = 27;
    pending.path = path;
    pending.annotate = annotate;
    pending.deadline = cap::Clock::now() + timeout;
    return pending;
}

// --- the state machine ------------------------------------------------------

void test_one_capture_in_flight() {
    cap::Tracker tracker;
    CHECK(!tracker.armed(), "a fresh tracker holds no capture");
    CHECK(!tracker.owns("C:/tmp/a.png"), "an unarmed tracker owns no path");

    CHECK(tracker.arm(request("C:/tmp/a.png", false, std::chrono::seconds(5))),
          "the first capture arms");
    CHECK(tracker.armed(), "arming is visible");
    CHECK(!tracker.arm(request("C:/tmp/b.png", false, std::chrono::seconds(5))),
          "a second capture is refused rather than queued");
    CHECK(tracker.request().path == "C:/tmp/a.png",
          "the refused capture did not replace the armed one");

    // A `shot_now` landing on another path is not ours, which is what stops an
    // agent request being completed by somebody else's screenshot.
    CHECK(tracker.owns("C:/tmp/a.png"), "the armed path is owned");
    CHECK(!tracker.owns("C:/tmp/b.png"), "another path is not owned");

    const cap::Request released = tracker.release();
    CHECK(released.request_id == "req-1" && released.ticket_id == 27,
          "release hands back the correlation ids");
    CHECK(!tracker.armed(), "release clears the armed state");
    CHECK(tracker.release().request_id.empty(),
          "releasing an unarmed tracker is a defined no-op");
    CHECK(tracker.arm(request("C:/tmp/b.png", false, std::chrono::seconds(5))),
          "a later capture can arm once the slot is free");
}

void test_capture_timeout() {
    cap::Tracker tracker;
    tracker.arm(request("C:/tmp/a.png", false, std::chrono::milliseconds(50)));
    // Read the deadline back rather than recomputing it: the request's clock
    // reading and this one are not the same instant.
    const auto deadline = tracker.request().deadline;
    CHECK(!tracker.expired(deadline - std::chrono::milliseconds(1)),
          "a capture inside its deadline is not expired");
    CHECK(tracker.expired(deadline),
          "the deadline is inclusive: reaching it expires the capture");
    CHECK(tracker.expired(deadline + std::chrono::seconds(1)),
          "a capture past its deadline stays expired");

    const cap::Terminal terminal = cap::terminal_for(cap::Resolution::TimedOut);
    CHECK(terminal.status == viewer::agent::Status::Timeout,
          "a capture deadline reports the protocol timeout code");
    CHECK(!terminal.message.empty(),
          "the timeout says what to do about it rather than only failing");

    tracker.release();
    CHECK(!tracker.expired(cap::Clock::now() + std::chrono::seconds(1)),
          "a released capture cannot expire a second time");
}

void test_failed_presentation() {
    // The editor stopped presenting: the shot deadman abandons the queued
    // capture. That is not the caller's input being wrong and not a handler
    // crashing, so it is not_ready rather than invalid_input or a failure.
    const cap::Terminal abandoned =
        cap::terminal_for(cap::Resolution::Abandoned);
    CHECK(abandoned.status == viewer::agent::Status::NotReady,
          "an abandoned capture reports not_ready");
    CHECK(abandoned.message.find("presenting") != std::string::npos,
          "the abandoned message names the presentation failure");

    // A frame DID present, but its readback or PNG write failed. That is a
    // genuine execution failure, and must stay distinct from the two above so
    // an agent can tell "retry" from "the editor is wedged".
    const cap::Terminal write_failed =
        cap::terminal_for(cap::Resolution::WriteFailed);
    CHECK(write_failed.status == viewer::agent::Status::ExecutionFailure,
          "a failed readback/write reports execution_failure");
    CHECK(abandoned.status != write_failed.status &&
              cap::terminal_for(cap::Resolution::TimedOut).status !=
                  abandoned.status,
          "the three failure causes stay distinguishable");
    CHECK(cap::terminal_for(cap::Resolution::Captured).status ==
                  viewer::agent::Status::Ok &&
              cap::terminal_for(cap::Resolution::Captured).message.empty(),
          "a landed capture is a plain ok with no failure text");
}

// --- geometry ---------------------------------------------------------------

void test_pick_from_capture_coherence() {
    const cap::Geometry geometry = docked_geometry();
    const cap::Rect image = cap::viewport_in_image(geometry);
    CHECK(near(image.x, 500.0) && near(image.y, 80.0) &&
              near(image.width, 1100.0) && near(image.height, 820.0),
          "the viewport rectangle scales into image pixels");

    // The round trip an agent actually performs: find a feature in the PNG,
    // map it back, hand it to viewport.pick.
    double logical_x = 0.0;
    double logical_y = 0.0;
    CHECK(cap::image_to_viewport_logical(geometry, 500.0, 80.0, logical_x,
                                         logical_y),
          "the viewport's top-left image pixel maps back");
    CHECK(near(logical_x, 0.0) && near(logical_y, 0.0),
          "the viewport's top-left image pixel is viewport-local (0,0)");

    CHECK(cap::image_to_viewport_logical(geometry, 1050.0, 500.0, logical_x,
                                         logical_y),
          "an interior image pixel maps back");
    CHECK(near(logical_x, 275.0) && near(logical_y, 210.0),
          "an interior image pixel divides by the framebuffer scale");

    double image_x = 0.0;
    double image_y = 0.0;
    CHECK(cap::viewport_logical_to_image(geometry, logical_x, logical_y, image_x,
                                         image_y),
          "the inverse mapping accepts what the forward one produced");
    CHECK(near(image_x, 1050.0) && near(image_y, 500.0),
          "image -> viewport -> image is the identity");

    // UI chrome is not the 3D view. Answering with the nearest edge instead
    // would hand back a pick coordinate for a pixel the caller never saw.
    CHECK(!cap::image_to_viewport_logical(geometry, 10.0, 10.0, logical_x,
                                          logical_y),
          "a pixel in the docked panels is refused, not clamped");
    CHECK(!cap::image_to_viewport_logical(geometry, 1600.0, 500.0, logical_x,
                                          logical_y),
          "the far edge is half-open, exactly like the pick bound");
    CHECK(!cap::viewport_logical_to_image(geometry, 550.0, 0.0, image_x, image_y),
          "a viewport-local coordinate at the width is out of bounds");

    // A cropped capture (issue replay) moves the image origin; the mapping has
    // to follow it rather than assume the PNG is the whole swapchain.
    cap::Geometry cropped = geometry;
    cropped.image_origin_x = 500.0;
    cropped.image_origin_y = 80.0;
    cropped.image_width = 1100;
    cropped.image_height = 820;
    CHECK(cap::image_to_viewport_logical(cropped, 0.0, 0.0, logical_x, logical_y),
          "a crop that starts at the viewport maps its own origin");
    CHECK(near(logical_x, 0.0) && near(logical_y, 0.0),
          "a cropped capture's (0,0) is the viewport's (0,0)");
}

void test_resize_between_arm_and_capture() {
    // Armed against the docked layout; captured after the window shrank to
    // 960x540 at 1x with a narrower viewport. Every number in the result comes
    // from the SECOND geometry, because that is the image on disk.
    const cap::Geometry armed = docked_geometry();
    cap::Geometry captured;
    captured.viewport_logical = cap::Rect{200.0, 30.0, 700.0, 470.0};
    captured.framebuffer_scale_x = 1.0;
    captured.framebuffer_scale_y = 1.0;
    captured.image_width = 960;
    captured.image_height = 540;

    double logical_x = 0.0;
    double logical_y = 0.0;
    CHECK(cap::image_to_viewport_logical(captured, 500.0, 200.0, logical_x,
                                         logical_y),
          "a pixel inside the resized viewport maps back");
    CHECK(near(logical_x, 300.0) && near(logical_y, 170.0),
          "the resized capture uses its own origin and scale");

    double stale_x = 0.0;
    double stale_y = 0.0;
    const bool stale_ok = cap::image_to_viewport_logical(armed, 500.0, 200.0,
                                                         stale_x, stale_y);
    CHECK(!stale_ok || !near(stale_x, logical_x) || !near(stale_y, logical_y),
          "the pre-resize geometry would have answered differently, which is "
          "why the capture frame's geometry is the one reported");

    const cap::Request pending =
        request("C:/tmp/a.png", false, std::chrono::seconds(5));
    matter::CameraDesc camera;
    viewer::agent::Context context;
    context.frame_id = 819;
    context.view_id = 819;
    const Value result =
        cap::capture_result_json(pending, captured, camera, context, nullptr);
    const Value& image = field(result, "image", "capture reports its image");
    CHECK(near(field(image, "width", "image width").num, 960.0) &&
              near(field(image, "height", "image height").num, 540.0),
          "the reported dimensions are the resized PNG's");
    const Value& viewport =
        field(result, "viewport", "capture reports its viewport");
    const Value& logical = field(viewport, "logical", "logical viewport rect");
    CHECK(near(field(logical, "width", "logical width").num, 700.0),
          "the reported viewport bounds are the resized ones");
}

void test_result_shape_and_revisions() {
    const cap::Geometry geometry = docked_geometry();
    const cap::Request pending =
        request("C:/tmp/shot.png", false, std::chrono::seconds(5));
    matter::CameraDesc camera;
    camera.position = {1.0f, 2.0f, 3.0f};
    viewer::agent::Context context;
    context.scene_ready = true;
    context.session_id = 1;
    context.session_generation = 2;
    context.scene_generation = 4;
    context.scene_revision = 12;
    context.selection_revision = 3;
    context.frame_id = 819;
    context.view_id = 819;

    const Value result =
        cap::capture_result_json(pending, geometry, camera, context, nullptr);
    CHECK(field(result, "path", "capture reports its path").str ==
              "C:/tmp/shot.png",
          "the capture echoes the written path");
    CHECK(field(result, "completion_marker", "capture reports its marker").str ==
              "C:/tmp/shot.png.done",
          "the capture names the .done marker the shot verbs also write");

    // Revisions are decimal STRINGS everywhere in this protocol; a frame serial
    // that survived to 2^53 must not be truncated into a double on the way out.
    const Value& captured =
        field(result, "captured", "capture reports the captured revisions");
    const Value& scene = field(captured, "scene", "captured scene block");
    CHECK(field(scene, "revision", "captured scene revision").kind ==
              Value::Kind::String,
          "captured revisions are decimal strings");
    CHECK(field(scene, "revision", "captured scene revision").str == "12",
          "the captured scene revision is the frame's, not a fresh read");
    const Value& selection =
        field(captured, "selection", "captured selection block");
    CHECK(field(selection, "revision", "captured selection revision").str == "3",
          "the captured selection revision travels with the image");
    const Value& presented =
        field(result, "presented", "capture reports the presented frame");
    CHECK(field(presented, "id", "presented frame id").str == "819" &&
              field(presented, "view_id", "presented view id").str == "819",
          "the presented frame/view identity is what expect.frame_id guards on");

    const Value& mapping =
        field(result, "pick_mapping", "capture states the pick mapping");
    const Value& offset = field(mapping, "image_offset", "mapping offset");
    CHECK(near(field(offset, "x", "mapping offset x").num, 500.0) &&
              near(field(offset, "y", "mapping offset y").num, 80.0),
          "the stated mapping matches viewport_in_image");

    const Value& annotations =
        field(result, "annotations", "annotations are always present");
    CHECK(field(annotations, "available", "annotation availability").b == false,
          "an un-annotated capture says so rather than omitting the key");
}

// --- annotations ------------------------------------------------------------

viewer::SelectionBounds unit_cube_at(float x, float y, float z) {
    viewer::SelectionBounds bounds{};
    bounds.local_min[0] = bounds.local_min[1] = bounds.local_min[2] = -0.5f;
    bounds.local_max[0] = bounds.local_max[1] = bounds.local_max[2] = 0.5f;
    const float identity[16] = {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) bounds.world_matrix[i] = identity[i];
    return bounds;
}

void test_annotations() {
    cap::Geometry geometry = docked_geometry();
    matter::CameraDesc camera;
    camera.position = {0.0f, 0.0f, 10.0f};
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};

    std::vector<cap::AnnotationInput> inputs;
    cap::AnnotationInput in_view;
    in_view.object = {viewer::agent::ObjectIdentity::Kind::Entity, 42};
    in_view.primary = true;
    in_view.resolved = true;
    in_view.bounds = unit_cube_at(0.0f, 0.0f, 0.0f);
    inputs.push_back(in_view);

    cap::AnnotationInput unresolved;
    unresolved.object = {viewer::agent::ObjectIdentity::Kind::BakedRoot, 7};
    unresolved.resolved = false;
    inputs.push_back(unresolved);

    cap::AnnotationInput behind;
    behind.object = {viewer::agent::ObjectIdentity::Kind::Entity, 43};
    behind.resolved = true;
    behind.bounds = unit_cube_at(0.0f, 0.0f, 40.0f);  // behind the eye
    inputs.push_back(behind);

    const std::vector<cap::Annotation> annotations =
        cap::project_annotations(inputs, camera, geometry);
    CHECK(annotations.size() == 3,
          "every selected object gets a row, resolved or not");

    const cap::Rect viewport = cap::viewport_in_image(geometry);
    CHECK(annotations[0].available && annotations[0].primary,
          "an in-view object is annotated and keeps its primary flag");
    CHECK(annotations[0].image_rect.x >= viewport.x &&
              annotations[0].image_rect.y >= viewport.y &&
              annotations[0].image_rect.x + annotations[0].image_rect.width <=
                  viewport.x + viewport.width &&
              annotations[0].image_rect.y + annotations[0].image_rect.height <=
                  viewport.y + viewport.height,
          "the annotation rectangle stays inside the captured viewport");
    CHECK(annotations[0].image_rect.width > 0.0 &&
              annotations[0].image_rect.height > 0.0,
          "a box facing the camera has a non-empty rectangle");
    // The centred cube is centred: the viewport-local rectangle straddles the
    // middle of the 3D view, which is what an agent aims viewport.pick at.
    const double centre_x = annotations[0].viewport_logical_rect.x +
                            annotations[0].viewport_logical_rect.width * 0.5;
    const double centre_y = annotations[0].viewport_logical_rect.y +
                            annotations[0].viewport_logical_rect.height * 0.5;
    CHECK(std::fabs(centre_x - geometry.viewport_logical.width * 0.5) < 1.0 &&
              std::fabs(centre_y - geometry.viewport_logical.height * 0.5) < 1.0,
          "a world-origin box annotates the centre of the viewport");
    double picked_x = 0.0;
    double picked_y = 0.0;
    CHECK(cap::image_to_viewport_logical(
              geometry, annotations[0].image_rect.x + 1.0,
              annotations[0].image_rect.y + 1.0, picked_x, picked_y),
          "an annotation's own pixels map back into pick coordinates");

    CHECK(!annotations[1].available &&
              annotations[1].reason.find("bounds") != std::string::npos,
          "an unresolved selection entry says why rather than vanishing");
    CHECK(!annotations[2].available &&
              annotations[2].reason.find("eye plane") != std::string::npos,
          "a box crossing the eye plane is refused, not given a wild rectangle");

    // Part Workbench isolation: the picture is real, the selection describes
    // the other world, so nothing is projected into it.
    geometry.production_view = false;
    const std::vector<cap::Annotation> isolated =
        cap::project_annotations(inputs, camera, geometry);
    CHECK(isolated.size() == 3 && !isolated[0].available &&
              isolated[0].reason.find("isolation") != std::string::npos,
          "an isolation frame annotates nothing and says why");

    // And the JSON keeps the typed identity plus a printable label.
    geometry.production_view = true;
    const cap::Request pending =
        request("C:/tmp/shot.png", true, std::chrono::seconds(5));
    viewer::agent::Context context;
    const Value result = cap::capture_result_json(
        pending, geometry, camera, context, &annotations);
    const Value& block = field(result, "annotations", "annotation block");
    CHECK(field(block, "available", "annotation availability").b,
          "a requested annotation block is available");
    const Value& rows = field(block, "objects", "annotation rows");
    CHECK(rows.kind == Value::Kind::Array && rows.arr.size() == 3,
          "one JSON row per selected object");
    const Value& object = field(rows.arr[0], "object", "annotation identity");
    CHECK(field(object, "kind", "annotation kind").str == "entity" &&
              field(object, "id", "annotation id").str == "42",
          "annotations carry the typed identity, id as a decimal string");
    CHECK(field(rows.arr[0], "label", "annotation label").str == "entity:42",
          "annotations carry a printable label for drawing on the image");
    CHECK(!field(rows.arr[1], "available", "unresolved row availability").b &&
              rows.arr[1].find("image_rect") == nullptr,
          "an unavailable row carries no rectangle to be mistaken for one");
}

// --- arguments --------------------------------------------------------------

Value string_value(const char* text) {
    Value value;
    value.kind = Value::Kind::String;
    value.str = text;
    return value;
}

void test_arguments() {
    Value arguments;
    arguments.kind = Value::Kind::Object;
    cap::Arguments parsed;
    std::string error;
    CHECK(!cap::parse_arguments(arguments, parsed, error) && !error.empty(),
          "path is required");

    arguments.set("path", string_value(""));
    CHECK(!cap::parse_arguments(arguments, parsed, error),
          "an empty path is rejected");

    arguments.set("path", string_value("C:/tmp/shot.png"));
    CHECK(cap::parse_arguments(arguments, parsed, error),
          "a path alone is enough");
    CHECK(parsed.path == "C:/tmp/shot.png" && !parsed.annotate,
          "annotations are off by default, so a capture cannot cost a bounds "
          "scan nobody asked for");

    Value annotate;
    annotate.kind = Value::Kind::Bool;
    annotate.b = true;
    arguments.set("annotate_selection", annotate);
    CHECK(cap::parse_arguments(arguments, parsed, error) && parsed.annotate,
          "annotate_selection is honoured");

    arguments.set("annotate_selection", string_value("true"));
    CHECK(!cap::parse_arguments(arguments, parsed, error) && !error.empty(),
          "a string annotate_selection is invalid_input, not truthy");
}

}  // namespace

int main() {
    test_one_capture_in_flight();
    test_capture_timeout();
    test_failed_presentation();
    test_pick_from_capture_coherence();
    test_resize_between_arm_and_capture();
    test_result_shape_and_revisions();
    test_annotations();
    test_arguments();
    std::printf("Viewport capture tests passed.\n");
    return 0;
}
